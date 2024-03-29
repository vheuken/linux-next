/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/
#ifndef __USB_OPS_LINUX_H__
#define __USB_OPS_LINUX_H__

#define VENDOR_CMD_MAX_DATA_LEN	254

#define RTW_USB_CONTROL_MSG_TIMEOUT_TEST	10/* ms */
#define RTW_USB_CONTROL_MSG_TIMEOUT	500/* ms */

#define MAX_USBCTRL_VENDORREQ_TIMES	10

#define RTW_USB_BULKOUT_TIME	5000/* ms */

#define _usbctrl_vendorreq_async_callback(urb, regs)	\
	_usbctrl_vendorreq_async_callback(urb)
#define usb_bulkout_zero_complete(purb, regs)		\
	usb_bulkout_zero_complete(purb)
#define usb_write_mem_complete(purb, regs)		\
	usb_write_mem_complete(purb)
#define usb_write_port_complete(purb, regs)		\
	usb_write_port_complete(purb)
#define usb_read_port_complete(purb, regs)		\
	usb_read_port_complete(purb)
#define usb_read_interrupt_complete(purb, regs)		\
	usb_read_interrupt_complete(purb)

unsigned int ffaddr2pipehdl(struct dvobj_priv *pdvobj, u32 addr);

u8 usb_read8(struct adapter *adapter, u32 addr);
u16 usb_read16(struct adapter *adapter, u32 addr);
u32 usb_read32(struct adapter *adapter, u32 addr);

u32 usb_read_port(struct adapter *adapter, u32 addr, u32 cnt, u8 *pmem);
void usb_read_port_cancel(struct adapter *adapter);

int usb_write8(struct adapter *adapter, u32 addr, u8 val);
int usb_write16(struct adapter *adapter, u32 addr, u16 val);
int usb_write32(struct adapter *adapter, u32 addr, u32 val);
int usb_writeN(struct adapter *adapter, u32 addr, u32 length, u8 *pdata);

u32 usb_write_port(struct adapter *adapter, u32 addr, u32 cnt, u8 *pmem);
void usb_write_port_cancel(struct adapter *adapter);

#endif
