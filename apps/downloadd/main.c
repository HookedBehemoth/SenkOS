/*
 * downloadd: system updates and the launcher's game packs, one at a time (root, respawned by
 * init: busybox-inittab).
 *
 * Clients call org.senkos.Updates1 on the system bus (apps/bus/org.senkos.Updates1.xml, shell and
 * root alone: org.senkos.Updates1.conf): Check, Update, Pack, Cancel, Jobs, and hear each job's
 * news as its Job signal.
 *
 * A pack is done when it's in /data/OS/assets/thumbnails (the launcher reloads on its Job). The
 * system update is done when the other slot holds it, armed for the next boot (ab write, ab arm):
 * rebooting finishes it, and the launcher commits it once it's up.
 *
 * The work is done by this same program run again, one process a job, which reports on stdout:
 * a pack's as player, the system's as root. The server and what's asked of it come from
 * /etc/updates ([Updates] Server, Channel: root's, /data/OS/overlay to change it), never from a
 * client: the system image is checked against the hash the server gives.
 *
 *     downloadd pack NAME | check | os      one job, by hand too
 *     downloadd zone                        the time zone this network's address is in, guessed
 *                                           by ipapi.co (for `clock`, which sets it unless one
 *                                           was chosen)
 */
#include <string.h>

#include "downloadd.h"

bool safe_pack(str s) {
  if (s.n == 0 || s.n > 80 || s.p[0] == '.')
    return false;
  for (size_t i = 0; i < s.n; i++)
    if ((unsigned char)s.p[i] < ' ' || s.p[i] == '/' || s.p[i] == '[' || s.p[i] == ']')
      return false;
  return true;
}

int main(int argc, char **argv) { return argc > 1 ? job(argc, argv) : daemon_main(); }
