/* Link-time stand-in for libc.so.6 (aarch64). Only symbol names and versions
 * matter; the real glibc on the device provides the code. */
void __libc_start_main(void) {}
void open(void) {}
void read(void) {}
void close(void) {}
void ioctl(void) {}
void readlink(void) {}
void memcpy(void) {}
void memset(void) {}
void memmove(void) {}
void memcmp(void) {}
