#ifndef __RSWAP_OPS_H
#define __RSWAP_OPS_H

int rswap_client_init(char *server_ip, int server_port, int mem_size);
void rswap_client_exit(void);

int rswap_register_frontswap(void);
int rswap_replace_frontswap(void);

#endif // __RSWAP_OPS_H
