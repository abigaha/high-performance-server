#include <errno.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol) {
  (void)domain;
  (void)type;
  (void)protocol;
  errno = EMFILE;
  return -1;
}
