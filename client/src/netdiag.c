/* Diagnosi della rete del MiSTer, scritta nel log quando qualcosa va storto:
 * stato del Wi-Fi, voce ARP del PC, ping e ultimi messaggi del kernel. */
#include "netdiag.h"

#include <stdio.h>
#include <string.h>

void mg_net_diag(const char *why, const char *host) {
  char cmd[512];
  /* host è un indirizzo IP o un nome dal .conf: niente caratteri da shell */
  for (const char *c = host; *c; c++)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-'))
      return;
  snprintf(cmd, sizeof(cmd),
           "date +%%T; echo uptime $(cut -d' ' -f1 /proc/uptime) s; ip route 2>&1 | head -3; ip -s neigh show %s 2>&1; "
           "for w in /sys/class/net/wlan*; do [ -e $w ] && iw dev $(basename $w) link 2>&1 | head -6; done; "
           "ping -c 2 -W 1 %s 2>&1 | tail -2; "
           "dmesg 2>/dev/null | grep -v -i -E 'MiSTer_fb|fpga' | tail -25",
           host, host);
  printf("[rete] %s: diagnosi\n", why);
  fflush(stdout);
  FILE *p = popen(cmd, "r");
  if (!p)
    return;
  char line[256];
  while (fgets(line, sizeof(line), p))
    printf("[rete]   %s", line);
  pclose(p);
  fflush(stdout);
}
