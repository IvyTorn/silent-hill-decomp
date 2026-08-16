/*
 * sh_log_n64.h - the N64 port's on-screen log ring.
 *
 * Every other port reads its log back off a disk. A cartridge cannot, and ares
 * surfaces IS-Viewer only in a GUI window of its own, so a headless run sees
 * nothing. SH_DBG output is teed into this ring and painted over the frame.
 */
#ifndef SH_LOG_N64_H
#define SH_LOG_N64_H

void        ShLogN64_Push(const char* line);
int         ShLogN64_Rows(void);
const char* ShLogN64_Row(int i);   /* oldest first */

int  ShLogN64_ScreenEnabled(void);
void ShLogN64_ScreenEnable(int on);

#endif /* SH_LOG_N64_H */
