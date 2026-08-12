#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

// Mesa/NVK supplies the secure_getenv fallback in its utility object.  Do not
// provide a second copy here: the static linker may pull both objects in when
// the PPSSPP frontend and NVK are linked together.

void* memalign(size_t alignment, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size) {
	if (!memptr || alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
		return EINVAL;
	}
	void* pointer = memalign(alignment, size);
	if (!pointer) {
		return ENOMEM;
	}
	*memptr = pointer;
	return 0;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
	const off_t original_offset = lseek(fd, 0, SEEK_CUR);
	if (original_offset == static_cast<off_t>(-1) ||
		lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
		return -1;
	}
	const ssize_t result = read(fd, buf, count);
	const int saved_errno = errno;
	(void)lseek(fd, original_offset, SEEK_SET);
	errno = saved_errno;
	return result;
}

int regcomp(void *preg, const char *regex, int cflags) {
	(void)preg;
	(void)regex;
	(void)cflags;
	return 0;
}

int regexec(const void *preg, const char *str, size_t nmatch, void *pmatch, int eflags) {
	(void)preg;
	(void)str;
	(void)nmatch;
	(void)pmatch;
	(void)eflags;
	return 1;
}

void regfree(void *preg) {
	(void)preg;
}

uid_t getuid() {
	return 0;
}

uid_t geteuid() {
	return 0;
}

gid_t getgid() {
	return 0;
}

gid_t getegid() {
	return 0;
}

int flock(int fd, int operation) {
	(void)fd;
	(void)operation;
	return 0;
}

int dirfd(void *dirp) {
	(void)dirp;
	errno = ENOTSUP;
	return -1;
}

int fstatat(int dirfd_, const char *path, struct stat *st, int flags) {
	(void)dirfd_;
#ifdef AT_SYMLINK_NOFOLLOW
	if (flags & AT_SYMLINK_NOFOLLOW) {
		return lstat(path, st);
	}
#else
	(void)flags;
#endif
	return stat(path, st);
}

int getpwuid_r(uid_t uid, void *pwd, char *buf, size_t buflen, void **result) {
	(void)uid;
	(void)pwd;
	(void)buf;
	(void)buflen;
	if (result) {
		*result = nullptr;
	}
	return 0;
}

long sysconf(int name) {
	switch (name) {
#ifdef _SC_PAGESIZE
	case _SC_PAGESIZE:
		return 0x1000;
#endif
#ifdef _SC_PAGE_SIZE
#if !defined(_SC_PAGESIZE) || _SC_PAGE_SIZE != _SC_PAGESIZE
	case _SC_PAGE_SIZE:
		return 0x1000;
#endif
#endif
#ifdef _SC_NPROCESSORS_ONLN
	case _SC_NPROCESSORS_ONLN:
		return 3;
#endif
#ifdef _SC_NPROCESSORS_CONF
	case _SC_NPROCESSORS_CONF:
		return 4;
#endif
#ifdef _SC_PHYS_PAGES
	case _SC_PHYS_PAGES:
		return (static_cast<uint64_t>(3072) << 20) / 0x1000;
#endif
	default:
		errno = EINVAL;
		return -1;
	}
}

int munmap(void *addr, size_t length) {
	(void)addr;
	(void)length;
	return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
	(void)how;
	(void)set;
	if (oldset) {
		*oldset = {};
	}
	return 0;
}

}
