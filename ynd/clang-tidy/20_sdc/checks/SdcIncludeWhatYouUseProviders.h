#pragma once

#include "llvm/ADT/StringRef.h"

namespace clang::tidy::sdc::include_what_you_use {

    struct HeaderExport {
        llvm::StringLiteral Header;
        llvm::StringLiteral Facade;
    };

    // Whole-header contracts required by C++ [cinttypes.syn] and C 7.8.
    inline constexpr HeaderExport HeaderExports[] = {
        {"<cstdint>", "<cinttypes>"},
        {"<stdint.h>", "<inttypes.h>"},
    };

    struct SymbolProvider {
        llvm::StringLiteral Name;
        llvm::StringLiteral Header;
    };

    // Explicit symbol-level facades supplement Clang's table without changing
    // its preferred headers. Unlisted transitive declarations are not exported.
    inline constexpr SymbolProvider StandardSymbolProviders[] = {
        // Public default traits: C++ [string.view.synop], [string.view.template].
        {"std::char_traits", "string_view"},

        // Public stream counts/offsets: C++ [stream.types], [ostream.unformatted],
        // [istream.unformatted] and [streambuf]. Extend the explicit stream-facade
        // policy to their numeric types, without exporting all of <ios>.
        {"std::streamsize", "istream"},
        {"std::streamsize", "ostream"},
        {"std::streamsize", "streambuf"},
        {"std::streamsize", "iostream"},
        {"std::streamsize", "sstream"},
        {"std::streamsize", "fstream"},
        {"std::streamoff", "istream"},
        {"std::streamoff", "ostream"},
        {"std::streamoff", "streambuf"},
        {"std::streamoff", "iostream"},
        {"std::streamoff", "sstream"},
        {"std::streamoff", "fstream"},

        // File/string stream facades expose their public base stream family,
        // including the char and wchar_t aliases. This is our explicit facade
        // policy, not a whole-header export of <istream>, <ostream> or <iosfwd>.
        // Public bases: C++ [ifstream], [ofstream], [fstream], [istringstream],
        // [ostringstream], [stringstream]. Aliases checked with libc++/libstdc++.
        {"std::basic_istream", "sstream"},
        {"std::basic_istream", "fstream"},
        {"std::istream", "sstream"},
        {"std::istream", "fstream"},
        {"std::wistream", "sstream"},
        {"std::wistream", "fstream"},
        {"std::basic_ostream", "sstream"},
        {"std::basic_ostream", "fstream"},
        {"std::ostream", "sstream"},
        {"std::ostream", "fstream"},
        {"std::wostream", "sstream"},
        {"std::wostream", "fstream"},
        {"std::basic_iostream", "sstream"},
        {"std::basic_iostream", "fstream"},
        {"std::iostream", "sstream"},
        {"std::iostream", "fstream"},
        {"std::wiostream", "sstream"},
        {"std::wiostream", "fstream"},
    };

    // Public type contracts supplementing Clang's ISO C/C++ symbol table.
    // Unless noted, audited against the public glibc 2.35 headers in each group,
    // their explicit type declarations in bits/ headers, and libc++ C wrappers.
    // The first entry for a name is preferred when Clang has no primary provider;
    // later entries are equally valid alternatives. These are symbol exports,
    // not promises to export every type transitively included by those headers.
    // The consumer restricts these mappings to existing global system TypeDecls.
    inline constexpr SymbolProvider SystemTypeProviders[] = {
        // POSIX size_t exporters: Linux man-pages size_t(3type), NOTES.
        // https://man7.org/linux/man-pages/man3/size_t.3type.html
        // Clang already covers the ISO headers and owns the primary suggestion.
        {"size_t", "sys/types.h"},
        {"size_t", "aio.h"},
        {"size_t", "glob.h"},
        {"size_t", "grp.h"},
        {"size_t", "iconv.h"},
        {"size_t", "monetary.h"},
        {"size_t", "mqueue.h"},
        {"size_t", "ndbm.h"},
        {"size_t", "pwd.h"},
        {"size_t", "regex.h"},
        {"size_t", "search.h"},
        {"size_t", "signal.h"},
        {"size_t", "strings.h"},
        {"size_t", "sys/mman.h"},
        {"size_t", "sys/msg.h"},
        {"size_t", "sys/sem.h"},
        {"size_t", "sys/shm.h"},
        {"size_t", "sys/socket.h"},
        {"size_t", "sys/uio.h"},
        {"size_t", "unistd.h"},
        {"size_t", "wordexp.h"},

        // File and directory APIs: <dirent.h>, <sys/stat.h>, <sys/types.h>,
        // <fcntl.h>, <unistd.h>, <stdio.h>, <sys/mman.h>, <sys/statvfs.h>.
        {"DIR", "dirent.h"},
        {"dirent", "dirent.h"},
        {"dirent64", "dirent.h"},
        {"stat", "sys/stat.h"},
        {"stat64", "sys/stat.h"},
        {"mode_t", "sys/types.h"},
        {"mode_t", "sys/stat.h"},
        {"mode_t", "fcntl.h"},
        {"off_t", "sys/types.h"},
        {"off_t", "stdio.h"},
        {"off_t", "unistd.h"},
        {"off_t", "sys/mman.h"},
        {"off_t", "cstdio"},
        {"off_t", "sys/stat.h"},
        {"off_t", "fcntl.h"},
        {"off64_t", "sys/types.h"},
        {"off64_t", "stdio.h"},
        {"off64_t", "cstdio"},
        {"ssize_t", "sys/types.h"},
        {"ssize_t", "unistd.h"},
        {"ssize_t", "stdio.h"},
        {"ssize_t", "cstdio"},
        {"uid_t", "sys/types.h"},
        {"uid_t", "unistd.h"},
        {"uid_t", "sys/stat.h"},
        {"uid_t", "signal.h"},
        {"gid_t", "sys/types.h"},
        {"gid_t", "unistd.h"},
        {"gid_t", "sys/stat.h"},
        {"dev_t", "sys/types.h"},
        {"dev_t", "sys/stat.h"},
        {"ino_t", "sys/types.h"},
        {"ino_t", "sys/stat.h"},
        {"nlink_t", "sys/types.h"},
        {"nlink_t", "sys/stat.h"},
        {"blkcnt_t", "sys/types.h"},
        {"blkcnt_t", "sys/stat.h"},
        {"blksize_t", "sys/types.h"},
        {"blksize_t", "sys/stat.h"},
        {"fsblkcnt_t", "sys/types.h"},
        {"fsblkcnt_t", "sys/statvfs.h"},
        {"fsfilcnt_t", "sys/types.h"},
        {"fsfilcnt_t", "sys/statvfs.h"},
        {"ino64_t", "sys/types.h"},
        {"blkcnt64_t", "sys/types.h"},
        {"fsblkcnt64_t", "sys/types.h"},
        {"fsfilcnt64_t", "sys/types.h"},
        {"loff_t", "sys/types.h"},
        {"statvfs", "sys/statvfs.h"},
        {"statvfs64", "sys/statvfs.h"},
        {"flock", "fcntl.h"},
        {"flock64", "fcntl.h"},

        // Process and IPC identifiers: <sys/types.h>, <unistd.h>, <sys/wait.h>,
        // <signal.h>, <sys/resource.h>, and <sys/ipc.h>.
        {"pid_t", "sys/types.h"},
        {"pid_t", "unistd.h"},
        {"pid_t", "sys/wait.h"},
        {"pid_t", "signal.h"},
        {"id_t", "sys/types.h"},
        {"id_t", "sys/wait.h"},
        {"id_t", "sys/resource.h"},
        {"key_t", "sys/types.h"},
        {"key_t", "sys/ipc.h"},

        // Socket and network APIs: <sys/socket.h>, <netinet/in.h>, <sys/un.h>,
        // <netdb.h>, and <sys/uio.h>; socket structures may live in bits/socket.h.
        // POSIX.1-2017 <arpa/inet.h> and <netinet/in.h> explicitly promise
        // these integer types; other <stdint.h> names are only incidental.
        // https://man7.org/linux/man-pages/man0/arpa_inet.h.0p.html
        // https://man7.org/linux/man-pages/man0/netinet_in.h.0p.html
        {"uint16_t", "arpa/inet.h"},
        {"uint32_t", "arpa/inet.h"},
        {"uint8_t", "netinet/in.h"},
        {"uint32_t", "netinet/in.h"},
        {"sockaddr", "sys/socket.h"},
        {"sockaddr_storage", "sys/socket.h"},
        {"socklen_t", "sys/socket.h"},
        {"sa_family_t", "sys/socket.h"},
        {"msghdr", "sys/socket.h"},
        {"cmsghdr", "sys/socket.h"},
        {"linger", "sys/socket.h"},
        {"in_addr_t", "netinet/in.h"},
        {"in_port_t", "netinet/in.h"},
        {"in_addr", "netinet/in.h"},
        {"in6_addr", "netinet/in.h"},
        {"sockaddr_in", "netinet/in.h"},
        {"sockaddr_in6", "netinet/in.h"},
        {"ipv6_mreq", "netinet/in.h"},
        {"sockaddr_un", "sys/un.h"},
        {"addrinfo", "netdb.h"},
        {"hostent", "netdb.h"},
        {"netent", "netdb.h"},
        {"protoent", "netdb.h"},
        {"servent", "netdb.h"},
        {"iovec", "sys/uio.h"},
        {"iovec", "sys/socket.h"},

        // Time APIs: <time.h>, <sys/types.h>, <sys/time.h>, <sys/select.h>,
        // <sys/resource.h>, and <unistd.h>; these share bits/types declarations.
        {"clockid_t", "time.h"},
        {"clockid_t", "sys/types.h"},
        {"clockid_t", "ctime"},
        {"timer_t", "time.h"},
        {"timer_t", "sys/types.h"},
        {"timer_t", "ctime"},
        {"timespec", "time.h"},
        {"timespec", "ctime"},
        {"timespec", "sys/select.h"},
        {"timespec", "pthread.h"},
        {"timeval", "sys/time.h"},
        {"timeval", "sys/select.h"},
        {"timeval", "sys/resource.h"},
        {"itimerspec", "time.h"},
        {"itimerspec", "ctime"},
        {"itimerval", "sys/time.h"},
        {"useconds_t", "sys/types.h"},
        {"useconds_t", "unistd.h"},
        {"suseconds_t", "sys/types.h"},
        {"suseconds_t", "sys/time.h"},
        {"suseconds_t", "sys/select.h"},

        // Thread and scheduling APIs: <pthread.h>, <sys/types.h>, <sched.h>,
        // and <semaphore.h>. glibc shares pthread typedefs through bits/pthreadtypes.h.
        {"pthread_t", "pthread.h"},
        {"pthread_t", "sys/types.h"},
        {"pthread_attr_t", "pthread.h"},
        {"pthread_attr_t", "sys/types.h"},
        {"pthread_mutex_t", "pthread.h"},
        {"pthread_mutex_t", "sys/types.h"},
        {"pthread_mutexattr_t", "pthread.h"},
        {"pthread_mutexattr_t", "sys/types.h"},
        {"pthread_cond_t", "pthread.h"},
        {"pthread_cond_t", "sys/types.h"},
        {"pthread_condattr_t", "pthread.h"},
        {"pthread_condattr_t", "sys/types.h"},
        {"pthread_once_t", "pthread.h"},
        {"pthread_once_t", "sys/types.h"},
        {"pthread_key_t", "pthread.h"},
        {"pthread_key_t", "sys/types.h"},
        {"pthread_rwlock_t", "pthread.h"},
        {"pthread_rwlock_t", "sys/types.h"},
        {"pthread_rwlockattr_t", "pthread.h"},
        {"pthread_rwlockattr_t", "sys/types.h"},
        {"pthread_spinlock_t", "pthread.h"},
        {"pthread_spinlock_t", "sys/types.h"},
        {"pthread_barrier_t", "pthread.h"},
        {"pthread_barrier_t", "sys/types.h"},
        {"pthread_barrierattr_t", "pthread.h"},
        {"pthread_barrierattr_t", "sys/types.h"},
        {"sched_param", "sched.h"},
        {"cpu_set_t", "sched.h"},
        {"sem_t", "semaphore.h"},

        // Signal APIs: <signal.h>, <sys/select.h>, and libc++'s <csignal> wrapper.
        {"sigset_t", "signal.h"},
        {"sigset_t", "csignal"},
        {"sigset_t", "sys/select.h"},
        {"siginfo_t", "signal.h"},
        {"siginfo_t", "csignal"},
        {"sigval", "signal.h"},
        {"sigval", "csignal"},
        {"sigevent", "signal.h"},
        {"sigevent", "csignal"},
        {"sigaction", "signal.h"},
        {"sigaction", "csignal"},
        {"stack_t", "signal.h"},
        {"stack_t", "csignal"},

        // Multiplexing APIs: <sys/select.h>, <poll.h>, and its <sys/poll.h> form.
        {"fd_set", "sys/select.h"},
        {"pollfd", "poll.h"},
        {"pollfd", "sys/poll.h"},
        {"nfds_t", "poll.h"},
        {"nfds_t", "sys/poll.h"},

        // Other public libc types: <regex.h>, <termios.h>, <sys/resource.h>,
        // <locale.h>, and libc++'s <clocale> wrapper.
        {"regex_t", "regex.h"},
        {"regmatch_t", "regex.h"},
        {"regoff_t", "regex.h"},
        {"termios", "termios.h"},
        {"tcflag_t", "termios.h"},
        {"cc_t", "termios.h"},
        {"speed_t", "termios.h"},
        {"rlimit", "sys/resource.h"},
        {"rlimit64", "sys/resource.h"},
        {"rusage", "sys/resource.h"},
        {"rlim_t", "sys/resource.h"},
        {"rlim64_t", "sys/resource.h"},
        {"locale_t", "locale.h"},
        {"locale_t", "clocale"},

        // Linux event APIs: <sys/epoll.h>, <sys/eventfd.h>, <sys/signalfd.h>,
        // and <sys/inotify.h>. The compiler determines target/feature availability.
        {"epoll_event", "sys/epoll.h"},
        {"epoll_data_t", "sys/epoll.h"},
        {"eventfd_t", "sys/eventfd.h"},
        {"signalfd_siginfo", "sys/signalfd.h"},
        {"inotify_event", "sys/inotify.h"},
    };

} // namespace clang::tidy::sdc::include_what_you_use
