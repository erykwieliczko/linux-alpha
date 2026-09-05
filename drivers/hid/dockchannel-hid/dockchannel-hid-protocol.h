/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef _DOCKCHANNEL_HID_PROTOCOL_H
#define _DOCKCHANNEL_HID_PROTOCOL_H

#include <linux/string.h>
#include <linux/types.h>

#define DCHID_CMD_RESET_INTERFACE	0x40
#define DCHID_POWER_METHOD_1		1
#define DCHID_POWER_METHOD_2		2
#define DCHID_POWER_STATE_OFF		0
#define DCHID_POWER_STATE_ON		2
#define DCHID_POWER_METHOD_2_CMD_SIZE	9

static inline void
dchid_pm2_command(u8 iface, bool has_changed,
		  u8 command[DCHID_POWER_METHOD_2_CMD_SIZE])
{
	memset(command, 0, DCHID_POWER_METHOD_2_CMD_SIZE);
	command[0] = DCHID_CMD_RESET_INTERFACE;
	command[1] = DCHID_POWER_METHOD_2;
	command[2] = iface;
	command[3] = DCHID_POWER_STATE_ON;
	command[4] = has_changed;
}

#endif /* _DOCKCHANNEL_HID_PROTOCOL_H */
