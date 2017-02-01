#include "WNLogging.h"

#define _GNU_SOURCE 1 // needed for O_NOFOLLOW and pread()/pwrite()

#include <assert.h>
#include <climits>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <time.h>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using std::string;
using std::vector;
using std::setw;
using std::setfill;
using std::hex;
using std::dec;
using std::min;
using std::ostream;
using std::ostringstream;
using std::FILE;
using std::fwrite;
using std::fclose;
using std::fflush;
using std::fprintf;
using std::perror;

namespace wn
{

static const size_t kMaxLogMessageLen = 2*1024;
static pthread_mutex_t g_cout_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE* g_current_log_file = NULL;
static LogSeverity FLAGS_minlogSeverity(LogSeverity_INFO);

struct LoggingInitializer
{
    LoggingInitializer() {}

    ~LoggingInitializer() {}
};
static LoggingInitializer initializer;

void SetCurrentLogFilename(const char* filename)
{
    pthread_mutex_lock(&g_cout_mutex);

    if (g_current_log_file) {
        fclose(g_current_log_file);
        g_current_log_file = NULL;
    }

    if (filename && strcmp(filename, "") != 0) {
        g_current_log_file = fopen(filename, "w");
    }

    pthread_mutex_unlock(&g_cout_mutex);
}

struct LogMessage::LogMessageData
{
    LogMessageData();
    ~LogMessageData();
    int preserved_errno_;
    char message_text_[kMaxLogMessageLen + 1];
    LogStream stream_;
    char severity_;
    int line_;
    void (LogMessage::*send_method_)();
    std::string* message_; // NULL or string to write message into
    size_t num_prefix_chars_; // # of chars of prefix in this message
    size_t num_chars_to_log_; // # of chars of msg to send to log
    size_t num_chars_to_syslog_; // # of chars of msg to send to syslog
    const char* basename_; // basename of file that called LOG
    const char* fullname_; // fullname of file that called LOG
    const char* functionname_; // function name that called LOG
    bool has_been_flushed_; // false => data has not been flushed
    bool first_fatal_; // true => this was first fatal msg
    time_t timestamp_; // absolute timestamp of this log line

private:
    LogMessageData(const LogMessageData&);
    void operator=(const LogMessageData&);
};

static const char* GetLogSeverityName(LogSeverity severity) {
    switch (severity)
    {
    case LogSeverity_TESTS:
        return "T";
    case LogSeverity_NEVER:
        return "N";
    case LogSeverity_DEBUG:
        return "D";
    case LogSeverity_INFO:
        return "I";
    case LogSeverity_WARNING:
        return "W";
    case LogSeverity_ERROR:
        return "E";
    case LogSeverity_FATAL:
        return "F";
    }
    return "U";
}

const char* const_basename(const char* filepath) {
    const char* base = strrchr(filepath, '/');
    return base ? (base + 1) : filepath;
}

LogMessage::LogMessageData::LogMessageData()
    : stream_(message_text_, kMaxLogMessageLen) {
}

LogMessage::LogMessageData::~LogMessageData() {}

LogMessage::LogMessage(const char* file, int line)
    : data_(NULL) {
    Init(file, "", line, LogSeverity_INFO, &LogMessage::SendToLog);
}

LogMessage::LogMessage(const char* file, const char* function, int line, LogSeverity severity)
    : data_(NULL) {
    Init(file, function, line, severity, &LogMessage::SendToLog);
}

void LogMessage::Init(const char* file,
                      const char* function,
                      int line, LogSeverity severity, void (LogMessage::*send_method)()) {
    data_ = new LogMessageData();
    data_->first_fatal_ = false;

    stream().fill('0');
    data_->preserved_errno_ = errno;
    data_->severity_ = severity;
    data_->line_ = line;
    data_->send_method_ = send_method;
    data_->basename_ = const_basename(file);

    data_->num_chars_to_log_ = 0;
    data_->num_chars_to_syslog_ = 0;
    data_->fullname_ = file;
    data_->functionname_ = function;
    data_->has_been_flushed_ = false;

    // const auto thread_id = std::this_thread::get_id();
    const void* thread_id = (void*)pthread_self();

    this->stream() << "[" << GetLogSeverityName(severity);

    struct tm tm_time; // Time of creation of LogMessage

    data_->timestamp_ = time(NULL);
    localtime_r(&data_->timestamp_, &tm_time);
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // print date / time
    this->stream() << ' ' << setw(2) << 1 + tm_time.tm_mon
                   << '/' << setw(2) << tm_time.tm_mday
                   << ' ' << setw(2) << tm_time.tm_hour
                   << ':' << setw(2) << tm_time.tm_min
                   << ':' << setw(2) << tm_time.tm_sec
                   << '.' << setw(3) << tv.tv_usec / 1000;

    // absolute time ms
    this->stream() << ' ' << data_->timestamp_;

    // filename and line:
    this->stream()
        << ' ' << setfill(' ') << setw(5) << thread_id
        << ' ' << data_->basename_;
    if (strcmp(data_->functionname_, "") != 0) {
        this->stream() << ' ' << data_->functionname_;
    }
    this->stream() << ':' << data_->line_ << "] ";

    data_->num_prefix_chars_ = data_->stream_.pcount();
}

LogMessage::~LogMessage() {
    Flush();
    delete data_;
}

int LogMessage::preserved_errno() const {
    return data_->preserved_errno_;
}

ostream& LogMessage::stream() {
    return data_->stream_;
}

// Flush buffered message, called by the destructor, or any other function
// that needs to synchronize the log.
void LogMessage::Flush() {
    if (data_->has_been_flushed_ || data_->severity_ < FLAGS_minlogSeverity)
    {
        return;
    }

    data_->num_chars_to_log_ = data_->stream_.pcount();
    data_->num_chars_to_syslog_ = data_->num_chars_to_log_ - data_->num_prefix_chars_;

    // Do we need to add a \n to the end of this message?
    bool append_newline = (data_->message_text_[data_->num_chars_to_log_ - 1] != '\n');
    char original_final_char = '\0';

    // If we do need to add a \n, we'll do it by violating the memory of the
    // ostrstream buffer.  This is quick, and we'll make sure to undo our
    // modification before anything else is done with the ostrstream.  It
    // would be preferable not to do things this way, but it seems to be
    // the best way to deal with this.
    if (append_newline)
    {
        original_final_char = data_->message_text_[data_->num_chars_to_log_];
        data_->message_text_[data_->num_chars_to_log_++] = '\n';
    }

    (this->*(data_->send_method_))();

    if (append_newline)
    {
        // Fix the ostrstream back how it was before we screwed with it.
        // It's 99.44% certain that we don't need to worry about doing this.
        data_->message_text_[data_->num_chars_to_log_ - 1] = original_final_char;
    }

    // If errno was already set before we enter the logging call, we'll
    // set it back to that value when we return from the logging call.
    // It happens often that we log an error message after a syscall
    // failure, which can potentially set the errno to some other
    // values.  We would like to preserve the original errno.
    if (data_->preserved_errno_ != 0)
    {
        errno = data_->preserved_errno_;
    }

    // Note that this message is now safely logged.  If we're asked to flush
    // again, as a result of destruction, say, we'll do nothing on future calls.
    data_->has_been_flushed_ = true;

    if (data_->severity_ == LogSeverity_FATAL)
    {
        abort();
    }
}

void LogMessage::SendToLog() {
    // do not log empty lines:
    if ((data_->num_chars_to_log_ - data_->num_prefix_chars_) > 1)
    {
        pthread_mutex_lock(&g_cout_mutex);
        std::cout.write(data_->message_text_, (size_t)(data_->num_chars_to_log_));

        if (g_current_log_file) {
            fwrite(data_->message_text_,
                   (size_t)(data_->num_chars_to_log_),
                   1,
                   g_current_log_file);
        }

        pthread_mutex_unlock(&g_cout_mutex);
    }
}
}
