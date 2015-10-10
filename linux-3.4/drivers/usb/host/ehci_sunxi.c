/*
 * drivers/usb/host/ehci_sunxi.c
 * (C) Copyright 2010-2015
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 * yangnaitian, 2011-5-24, create this file
 * javen, 2011-6-26, add suspend and resume
 * javen, 2011-7-18, move clock and power operations out from driver
 *
 * SoftWinner EHCI Driver
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#include <linux/platform_device.h>
#include <linux/time.h>
#include <linux/timer.h>

#include <mach/sys_config.h>
#include <linux/clk.h>

#include <linux/arisc/arisc.h>
#include <linux/power/aw_pm.h>
#include <linux/power/scenelock.h>
#include "sunxi_hci.h"

static struct scene_lock  ehci_standby_lock[4];

//#define  SUNXI_USB_EHCI_DEBUG

#define  SUNXI_EHCI_NAME		"sunxi-ehci"
static const char ehci_name[] = SUNXI_EHCI_NAME;

static struct sunxi_hci_hcd *g_sunxi_ehci[4];
static u32 ehci_first_probe[4] = {1, 1, 1, 1};
static u32 ehci_enable[4] = {1, 1, 1, 1};

#ifdef CONFIG_USB_HCD_ENHANCE
extern atomic_t hci1_thread_scan_flag;
extern atomic_t hci3_thread_scan_flag;
#endif

extern int usb_disabled(void);
int sunxi_usb_disable_ehci(__u32 usbc_no);
int sunxi_usb_enable_ehci(__u32 usbc_no);

static void  ehci_set_interrupt_enable(const struct ehci_hcd *ehci, __u32 regs, u32 enable)
{
	ehci_writel(ehci, enable & 0x3f, (__u32 __iomem *)(regs + EHCI_OPR_USBINTR));
}

static void ehci_disable_periodic_schedule(const struct ehci_hcd *ehci, __u32 regs)
{
	u32 reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val	&= ~(0x1<<4);
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
}

static void ehci_disable_async_schedule(const struct ehci_hcd *ehci, __u32 regs)
{
	unsigned int reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val &= ~(0x1<<5);
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
}

static void ehci_set_config_flag(const struct ehci_hcd *ehci, __u32 regs)
{
	ehci_writel(ehci, 0x1, (__u32 __iomem *)(regs + EHCI_OPR_CFGFLAG));
}

static void ehci_test_stop(const struct ehci_hcd *ehci, __u32 regs)
{
	unsigned int reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val &= (~0x1);
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
}

static void ehci_test_reset(const struct ehci_hcd *ehci, __u32 regs)
{
	u32 reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val	|= (0x1<<1);
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
}

static unsigned int ehci_test_reset_complete(const struct ehci_hcd *ehci, __u32  regs)
{

	unsigned int reg_val = 0;

	reg_val = ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val &= (0x1<<1);

	return !reg_val;
}

static void ehci_start(const struct ehci_hcd *ehci, __u32 regs)
{
	unsigned int reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
	reg_val	|= 0x1;
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD));
}

static unsigned int ehci_is_halt(const struct ehci_hcd *ehci, __u32 regs)
{

	unsigned int reg_val = 0;
	reg_val = ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_USBSTS))  >> 12;
	reg_val &= 0x1;
	return reg_val;
}

static void ehci_port_control(const struct ehci_hcd *ehci, __u32 regs, u32 port_no, u32 control)
{
	ehci_writel(ehci, control, (__u32 __iomem *)(regs + EHCI_OPR_USBCMD + (port_no<<2)));
}

static void  ehci_put_port_suspend(const struct ehci_hcd *ehci, __u32 regs)
{
	unsigned int reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_PORTSC));
	reg_val	|= (0x01<<7);
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_PORTSC));
}
static void ehci_test_mode(const struct ehci_hcd *ehci, __u32 regs, u32 test_mode)
{
	unsigned int reg_val = 0;
	reg_val =  ehci_readl(ehci, (__u32 __iomem *)(regs + EHCI_OPR_PORTSC));
	reg_val &= ~(0x0f<<16);
	reg_val |= test_mode;
	ehci_writel(ehci, reg_val, (__u32 __iomem *)(regs + EHCI_OPR_PORTSC));
}

static void __ehci_ed_test(const struct ehci_hcd *ehci, __u32 regs, __u32 test_mode)
{
	ehci_set_interrupt_enable(ehci, regs, 0x00);
	ehci_disable_periodic_schedule(ehci, regs);
	ehci_disable_async_schedule(ehci, regs);

	ehci_set_config_flag(ehci, regs);

	ehci_test_stop(ehci, regs);
	ehci_test_reset(ehci, regs);
	while(!ehci_test_reset_complete(ehci, regs));  //Wait until EHCI reset complete

	if(!ehci_is_halt(ehci, regs))
	{
		DMSG_ERR("%s_%d\n", __func__, __LINE__);
	}

	ehci_start(ehci, regs);
	while(ehci_is_halt(ehci, regs));  //Wait until EHCI to be not halt

	/*Ehci Start, Config to Test*/
	ehci_set_config_flag(ehci, regs);
	ehci_port_control(ehci, regs, 0, EHCI_PORTSC_POWER);

	ehci_disable_periodic_schedule(ehci, regs);
	ehci_disable_async_schedule(ehci, regs);

	//Put Port Suspend
	ehci_put_port_suspend(ehci, regs);

	ehci_test_stop(ehci, regs);

	while((!ehci_is_halt(ehci, regs)));

	//Test Pack
	DMSG_INFO("Start Host Test,mode:0x%x!\n", test_mode);
	ehci_test_mode(ehci, regs, test_mode);
	DMSG_INFO("End Host Test,mode:0x%x!\n", test_mode);

}

static ssize_t show_ed_test(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t ehci_ed_test(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	__u32 test_mode = 0;
	struct usb_hcd *hcd = NULL;
	struct ehci_hcd *ehci = NULL;

	if(dev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return 0;
	}

	hcd = dev_get_drvdata(dev);
	if(hcd == NULL){
		DMSG_PANIC("ERR: hcd is null\n");
		return 0;
	}

	ehci = hcd_to_ehci(hcd);
	if(ehci == NULL){
		DMSG_PANIC("ERR: ehci is null\n");
		return 0;
	}

	mutex_lock(&dev->mutex);

	DMSG_INFO("ehci_ed_test:%s\n", buf);

	if(!strncmp(buf, "test_not_operating", 18)){
		test_mode = EHCI_PORTSC_PTC_DIS;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_j_state", 12)){
		test_mode = EHCI_PORTSC_PTC_J;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_k_state", 12)){
		test_mode = EHCI_PORTSC_PTC_K;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_se0_nak", 12)){
		test_mode = EHCI_PORTSC_PTC_SE0NAK;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_pack", 9)){
		test_mode = EHCI_PORTSC_PTC_PACKET;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_force_enable", 17)){
		test_mode = EHCI_PORTSC_PTC_FORCE;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else if(!strncmp(buf, "test_mask", 9)){
		test_mode = EHCI_PORTSC_PTC_MASK;
		DMSG_INFO("test_mode:0x%x\n", test_mode);
	}else {
		DMSG_PANIC("ERR: test_mode Argment is invalid\n");
		mutex_unlock(&dev->mutex);
		return count;
	}

	DMSG_INFO("regs: 0x%p\n",hcd->regs);
	__ehci_ed_test(ehci, (__u32 __force)hcd->regs, test_mode);

	mutex_unlock(&dev->mutex);

	return count;
}
static DEVICE_ATTR(ed_test, 0644, show_ed_test, ehci_ed_test);

static ssize_t ehci_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;

	if(dev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return 0;
	}

	sunxi_ehci = dev->platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sw_ehci is null\n");
		return 0;
	}

	return sprintf(buf, "ehci:%d, probe:%u, enable:%d\n", sunxi_ehci->usbc_no, sunxi_ehci->probe, ehci_enable[sunxi_ehci->usbc_no]);
}

static ssize_t ehci_enable_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;
	int value = 0;

	if(dev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return 0;
	}

	sunxi_ehci = dev->platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sw_ehci is null\n");
		return 0;
	}

	sunxi_ehci->host_init_state = 0;
	ehci_first_probe[sunxi_ehci->usbc_no] = 0;

	sscanf(buf, "%d", &value);
	if(value == 1){
		ehci_enable[sunxi_ehci->usbc_no] = 0;
		sunxi_ehci->hsic_enable_flag = 1;
		sunxi_usb_enable_ehci(sunxi_ehci->usbc_no);
		if(sunxi_ehci->hsic_ctrl_flag){
			sunxi_set_host_hisc_rdy(sunxi_ehci, 1);
		}
	}else if(value == 0){
		if(sunxi_ehci->hsic_ctrl_flag){
			sunxi_set_host_hisc_rdy(sunxi_ehci, 0);
		}

		ehci_enable[sunxi_ehci->usbc_no] = 1;
		sunxi_usb_disable_ehci(sunxi_ehci->usbc_no);
		sunxi_ehci->hsic_enable_flag = 0;
		ehci_enable[sunxi_ehci->usbc_no] = 0;
	}else{
		DMSG_INFO("unkown value (%d)\n", value);
	}

	return count;
}

static DEVICE_ATTR(ehci_enable, 0664, ehci_enable_show, ehci_enable_store);

static void sunxi_hcd_board_set_vbus(struct sunxi_hci_hcd *sunxi_ehci, int is_on)
{
	sunxi_ehci->set_power(sunxi_ehci, is_on);

	return;
}

static int open_ehci_clock(struct sunxi_hci_hcd *sunxi_ehci)
{
	return sunxi_ehci->open_clock(sunxi_ehci, 0);
}

static int close_ehci_clock(struct sunxi_hci_hcd *sunxi_ehci)
{
	return sunxi_ehci->close_clock(sunxi_ehci, 0);
}

static void sunxi_ehci_port_configure(struct sunxi_hci_hcd *sunxi_ehci, u32 enable)
{
	sunxi_ehci->port_configure(sunxi_ehci, enable);
}

static int sunxi_get_io_resource(struct platform_device *pdev, struct sunxi_hci_hcd *sunxi_ehci)
{
	return 0;
}

static int sunxi_release_io_resource(struct platform_device *pdev, struct sunxi_hci_hcd *sunxi_ehci)
{
	return 0;
}

static void sunxi_start_ehci(struct sunxi_hci_hcd *sunxi_ehci)
{

	sunxi_ehci->set_usbc_regulator(sunxi_ehci, 1);
#if defined (CONFIG_ARCH_SUN8IW6) || defined (CONFIG_ARCH_SUN9IW1)
	sunxi_ehci->hci_phy_ctrl(sunxi_ehci, 1);
#endif
	open_ehci_clock(sunxi_ehci);
	sunxi_ehci->usb_passby(sunxi_ehci, 1);
	sunxi_ehci_port_configure(sunxi_ehci, 1);
	sunxi_hcd_board_set_vbus(sunxi_ehci, 1);
}

static void sunxi_stop_ehci(struct sunxi_hci_hcd *sunxi_ehci)
{
	sunxi_hcd_board_set_vbus(sunxi_ehci, 0);
	sunxi_ehci_port_configure(sunxi_ehci, 0);
	sunxi_ehci->usb_passby(sunxi_ehci, 0);
	close_ehci_clock(sunxi_ehci);
#if defined (CONFIG_ARCH_SUN8IW6) || defined (CONFIG_ARCH_SUN9IW1)
	sunxi_ehci->hci_phy_ctrl(sunxi_ehci, 0);
#endif
	sunxi_ehci->set_usbc_regulator(sunxi_ehci, 0);

}

static int sunxi_ehci_setup(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	int ret = ehci_init(hcd);

	ehci->need_io_watchdog = 0;

	return ret;
}

static const struct hc_driver sunxi_ehci_hc_driver = {
	.description			= hcd_name,
	.product_desc			= "SW USB2.0 'Enhanced' Host Controller (EHCI) Driver",
	.hcd_priv_size			= sizeof(struct ehci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq				=  ehci_irq,
	.flags				=  HCD_MEMORY | HCD_USB2,

	/*
	 * basic lifecycle operations
	 *
	 * FIXME -- ehci_init() doesn't do enough here.
	 * See ehci-ppc-soc for a complete implementation.
	 */
	.reset				= sunxi_ehci_setup,
	.start				= ehci_run,
	.stop				= ehci_stop,
	.shutdown			= ehci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue			= ehci_urb_enqueue,
	.urb_dequeue			= ehci_urb_dequeue,
	.endpoint_disable		= ehci_endpoint_disable,
	.endpoint_reset			= ehci_endpoint_reset,

	/*
	 * scheduling support
	 */
	.get_frame_number		= ehci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data		= ehci_hub_status_data,
	.hub_control			= ehci_hub_control,
	.bus_suspend			= ehci_bus_suspend,
	.bus_resume				= ehci_bus_resume,
	.relinquish_port		= ehci_relinquish_port,
	.port_handed_over		= ehci_port_handed_over,

	.clear_tt_buffer_complete	= ehci_clear_tt_buffer_complete,
};

static int sunxi_ehci_hcd_probe(struct platform_device *pdev)
{
	struct usb_hcd 	*hcd 	= NULL;
	struct ehci_hcd *ehci	= NULL;
	struct sunxi_hci_hcd *sunxi_ehci = NULL;
	int ret = 0;

	if(pdev == NULL){
		DMSG_PANIC("ERR: Argment is invaild\n");
		return -1;
	}

	/* if usb is disabled, can not probe */
	if (usb_disabled()) {
		DMSG_PANIC("ERR: usb hcd is disabled\n");
		return -ENODEV;
	}

	sunxi_ehci = pdev->dev.platform_data;
	if(!sunxi_ehci){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		ret = -ENOMEM;
		goto ERR1;
	}

	sunxi_ehci->pdev = pdev;
	g_sunxi_ehci[sunxi_ehci->usbc_no] = sunxi_ehci;

	DMSG_INFO("[%s%d]: probe, pdev->name: %s, pdev->id: %d, sunxi_ehci: 0x%p\n",
		ehci_name, sunxi_ehci->usbc_no, pdev->name, pdev->id, sunxi_ehci);

	/* get io resource */
	sunxi_get_io_resource(pdev, sunxi_ehci);
	sunxi_ehci->ehci_base 		= sunxi_ehci->usb_vbase + SUNXI_USB_EHCI_BASE_OFFSET;
	sunxi_ehci->ehci_reg_length 	= SUNXI_USB_EHCI_LEN;

	/* creat a usb_hcd for the ehci controller */
	hcd = usb_create_hcd(&sunxi_ehci_hc_driver, &pdev->dev, ehci_name);
	if (!hcd){
		DMSG_PANIC("ERR: usb_create_hcd failed\n");
		ret = -ENOMEM;
		goto ERR2;
	}

	hcd->rsrc_start = (u32 __force)sunxi_ehci->ehci_base;
	hcd->rsrc_len 	= sunxi_ehci->ehci_reg_length;
	hcd->regs 	= sunxi_ehci->ehci_base;
	sunxi_ehci->hcd	= hcd;

	/* echi start to work */
	sunxi_start_ehci(sunxi_ehci);

	ehci = hcd_to_ehci(hcd);
	ehci->caps = hcd->regs;
	ehci->regs = hcd->regs + HC_LENGTH(ehci, readl(&ehci->caps->hc_capbase));

	/* cache this readonly data, minimize chip reads */
	ehci->hcs_params = readl(&ehci->caps->hcs_params);

	ret = usb_add_hcd(hcd, sunxi_ehci->irq_no, IRQF_DISABLED | IRQF_SHARED);
	if (ret != 0) {
		DMSG_PANIC("ERR: usb_add_hcd failed\n");
		ret = -ENOMEM;
		goto ERR3;
	}

	platform_set_drvdata(pdev, hcd);

	device_create_file(&pdev->dev, &dev_attr_ed_test);

#ifndef USB_SYNC_SUSPEND
	device_enable_async_suspend(&pdev->dev);
#endif

	sunxi_ehci->probe = 1;

	if(sunxi_ehci->not_suspend){
		scene_lock_init(&ehci_standby_lock[sunxi_ehci->usbc_no], SCENE_USB_STANDBY,  "ehci_standby");
	}

	/* Disable ehci, when driver probe */
	if(sunxi_ehci->host_init_state == 0){
		if(ehci_first_probe[sunxi_ehci->usbc_no]){
			sunxi_usb_disable_ehci(sunxi_ehci->usbc_no);
			ehci_first_probe[sunxi_ehci->usbc_no] = 0;
		}
	}

	if(ehci_enable[sunxi_ehci->usbc_no]){
		device_create_file(&pdev->dev, &dev_attr_ehci_enable);
		ehci_enable[sunxi_ehci->usbc_no] = 0;
	}

	return 0;

ERR3:
	usb_put_hcd(hcd);
	sunxi_stop_ehci(sunxi_ehci);
ERR2:
	sunxi_ehci->hcd = NULL;
	g_sunxi_ehci[sunxi_ehci->usbc_no] = NULL;

ERR1:
	return ret;
}

static int sunxi_ehci_hcd_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = NULL;
	struct sunxi_hci_hcd *sunxi_ehci = NULL;

	if(pdev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return -1;
	}

	hcd = platform_get_drvdata(pdev);
	if(hcd == NULL){
		DMSG_PANIC("ERR: hcd is null\n");
		return -1;
	}

	sunxi_ehci = pdev->dev.platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return -1;
	}

	DMSG_INFO("[%s%d]: remove, pdev->name: %s, pdev->id: %d, sunxi_ehci: 0x%p\n",
		ehci_name, sunxi_ehci->usbc_no, pdev->name, pdev->id, sunxi_ehci);

	if(ehci_enable[sunxi_ehci->usbc_no] == 0){
		device_remove_file(&pdev->dev, &dev_attr_ehci_enable);
	}

	device_remove_file(&pdev->dev, &dev_attr_ed_test);

	if(sunxi_ehci->not_suspend){
		scene_lock_destroy(&ehci_standby_lock[sunxi_ehci->usbc_no]);
	}
	usb_remove_hcd(hcd);

	sunxi_release_io_resource(pdev, sunxi_ehci);

	usb_put_hcd(hcd);

	sunxi_stop_ehci(sunxi_ehci);
	sunxi_ehci->probe = 0;

	sunxi_ehci->hcd = NULL;

#if !defined (CONFIG_ARCH_SUN8IW8) && !defined (CONFIG_ARCH_SUN8IW7) && !defined (CONFIG_USB_HCD_ENHANCE)
	if(sunxi_ehci->host_init_state){
		g_sunxi_ehci[sunxi_ehci->usbc_no] = NULL;
	}
#endif

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static void sunxi_ehci_hcd_shutdown(struct platform_device* pdev)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;

	if(pdev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return;
	}

	sunxi_ehci = pdev->dev.platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return;
	}

	if(sunxi_ehci->probe == 0){
		DMSG_PANIC("ERR: sunxi_ehci is disable, need not shutdown\n");
		return;
	}

	DMSG_INFO("[%s]: ehci shutdown start\n", sunxi_ehci->hci_name);
#ifdef CONFIG_USB_HCD_ENHANCE
				if(sunxi_ehci->usbc_no == 1){
					atomic_set(&hci1_thread_scan_flag, 0);
				}

				if(sunxi_ehci->usbc_no == 3){
					atomic_set(&hci3_thread_scan_flag, 0);
				}
#endif

	if(sunxi_ehci->not_suspend){
		scene_lock_destroy(&ehci_standby_lock[sunxi_ehci->usbc_no]);
	}
	usb_hcd_platform_shutdown(pdev);

	sunxi_stop_ehci(sunxi_ehci);

	DMSG_INFO("[%s]: ehci shutdown end\n", sunxi_ehci->hci_name);

	return ;
}

#ifdef CONFIG_PM

static int sunxi_ehci_hcd_suspend(struct device *dev)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;
	struct usb_hcd *hcd = NULL;
	struct ehci_hcd *ehci = NULL;
	unsigned long flags = 0;
	int val = 0;

	if(dev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return 0;
	}

	hcd = dev_get_drvdata(dev);
	if(hcd == NULL){
		DMSG_PANIC("ERR: hcd is null\n");
		return 0;
	}

	sunxi_ehci = dev->platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return 0;
	}

	if(sunxi_ehci->probe == 0){
		DMSG_PANIC("ERR: sunxi_ehci is disable, can not suspend\n");
		return 0;
	}

	ehci = hcd_to_ehci(hcd);
	if(ehci == NULL){
		DMSG_PANIC("ERR: ehci is null\n");
		return 0;
	}

#ifdef CONFIG_USB_HCD_ENHANCE
	atomic_set(&hci1_thread_scan_flag, 0);
	atomic_set(&hci3_thread_scan_flag, 0);
#endif
	if(sunxi_ehci->not_suspend){
		DMSG_INFO("[%s]: not suspend\n", sunxi_ehci->hci_name);
		enable_wakeup_src(CPUS_USBMOUSE_SRC, 0);
		scene_lock(&ehci_standby_lock[sunxi_ehci->usbc_no]);

		val = ehci_readl(ehci, &ehci->regs->intr_enable);
		val |= (0x7 << 0);
		ehci_writel(ehci, val, &ehci->regs->intr_enable);

#if defined (CONFIG_ARCH_SUN8IW6) || defined (CONFIG_ARCH_SUN9IW1)
			sunxi_ehci->hci_phy_ctrl(sunxi_ehci, 0);
#endif

	}else{
		DMSG_INFO("[%s]: sunxi_ehci_hcd_suspend\n", sunxi_ehci->hci_name);

		spin_lock_irqsave(&ehci->lock, flags);
		ehci_prepare_ports_for_controller_suspend(ehci, device_may_wakeup(dev));
		ehci_writel(ehci, 0, &ehci->regs->intr_enable);
		(void)ehci_readl(ehci, &ehci->regs->intr_enable);

		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

		spin_unlock_irqrestore(&ehci->lock, flags);

		sunxi_stop_ehci(sunxi_ehci);
	}

	return 0;
}

static int sunxi_ehci_hcd_resume(struct device *dev)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;
	struct usb_hcd *hcd = NULL;
	struct ehci_hcd *ehci = NULL;

	if(dev == NULL){
		DMSG_PANIC("ERR: Argment is invalid\n");
		return 0;
	}

	hcd = dev_get_drvdata(dev);
	if(hcd == NULL){
		DMSG_PANIC("ERR: hcd is null\n");
		return 0;
	}

	sunxi_ehci = dev->platform_data;
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return 0;
	}

	if(sunxi_ehci->probe == 0){
		DMSG_PANIC("ERR: sunxi_ehci is disable, can not resume\n");
		return 0;
	}

	ehci = hcd_to_ehci(hcd);
	if(ehci == NULL){
		DMSG_PANIC("ERR: ehci is null\n");
		return 0;
	}

	if(sunxi_ehci->not_suspend){
		DMSG_INFO("[%s]: controller not suspend, need not resume\n", sunxi_ehci->hci_name);
#if defined (CONFIG_ARCH_SUN8IW6) || defined (CONFIG_ARCH_SUN9IW1)
		sunxi_ehci->hci_phy_ctrl(sunxi_ehci, 1);
#endif
		scene_unlock(&ehci_standby_lock[sunxi_ehci->usbc_no]);
		disable_wakeup_src(CPUS_USBMOUSE_SRC, 0);
#ifdef CONFIG_USB_HCD_ENHANCE
		if(sunxi_ehci->usbc_no == 1){
			atomic_set(&hci1_thread_scan_flag, 1);
		}

		if(sunxi_ehci->usbc_no == 3){
			atomic_set(&hci3_thread_scan_flag, 1);
		}
#endif
	}else{
		DMSG_INFO("[%s]: sunxi_ehci_hcd_resume\n", sunxi_ehci->hci_name);
#ifndef CONFIG_ARCH_SUN9IW1
#ifdef  SUNXI_USB_FPGA
		fpga_config_use_hci((__u32 __force)sunxi_ehci->sram_vbase);
#endif
#endif
		sunxi_start_ehci(sunxi_ehci);

		/* Mark hardware accessible again as we are out of D3 state by now */
		set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

		if (ehci_readl(ehci, &ehci->regs->configured_flag) == FLAG_CF) {
			int	mask = INTR_MASK;

			ehci_prepare_ports_for_controller_resume(ehci);

			if (!hcd->self.root_hub->do_remote_wakeup){
				mask &= ~STS_PCD;
			}

			ehci_writel(ehci, mask, &ehci->regs->intr_enable);
			ehci_readl(ehci, &ehci->regs->intr_enable);

			return 0;
		}

		DMSG_INFO("[%s]: lost power, restarting\n", sunxi_ehci->hci_name);

		usb_root_hub_lost_power(hcd->self.root_hub);

		/* Else reset, to cope with power loss or flush-to-storage
		 * style "resume" having let BIOS kick in during reboot.
		 */
		(void) ehci_halt(ehci);
		(void) ehci_reset(ehci);

		/* emptying the schedule aborts any urbs */
		spin_lock_irq(&ehci->lock);
		if (ehci->reclaim)
			end_unlink_async(ehci);
		ehci_work(ehci);
		spin_unlock_irq(&ehci->lock);

		ehci_writel(ehci, ehci->command, &ehci->regs->command);
		ehci_writel(ehci, FLAG_CF, &ehci->regs->configured_flag);
		ehci_readl(ehci, &ehci->regs->command);	/* unblock posted writes */

		/* here we "know" root ports should always stay powered */
		ehci_port_power(ehci, 1);

		hcd->state = HC_STATE_SUSPENDED;
	}

	return 0;
}

static const struct dev_pm_ops  aw_ehci_pmops = {
	.suspend	= sunxi_ehci_hcd_suspend,
	.resume		= sunxi_ehci_hcd_resume,
};

#define SUNXI_EHCI_PMOPS 	&aw_ehci_pmops

#else

#define SUNXI_EHCI_PMOPS 	NULL

#endif

static struct platform_driver sunxi_ehci_hcd_driver ={
	.probe  = sunxi_ehci_hcd_probe,
	.remove	= sunxi_ehci_hcd_remove,
	.shutdown = sunxi_ehci_hcd_shutdown,
	.driver = {
			.name	= ehci_name,
			.owner	= THIS_MODULE,
			.pm	= SUNXI_EHCI_PMOPS,
		}
};

int sunxi_usb_disable_ehci(__u32 usbc_no)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;

#ifdef CONFIG_USB_HCD_ENHANCE
	if(usbc_no == 1){
		atomic_set(&hci1_thread_scan_flag, 0);
	}

	if(usbc_no == 3){
		atomic_set(&hci3_thread_scan_flag, 0);
	}
#endif

	sunxi_ehci = g_sunxi_ehci[usbc_no];
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return -1;
	}

#if !defined (CONFIG_ARCH_SUN8IW8) && !defined (CONFIG_ARCH_SUN8IW7) && !defined (CONFIG_USB_HCD_ENHANCE)
	if(sunxi_ehci->host_init_state){
		DMSG_PANIC("ERR: not support sunxi_usb_disable_ehci\n");
		return -1;
	}
#endif
	if(sunxi_ehci->probe == 0){
		DMSG_PANIC("ERR: sunxi_ehci is disable, can not disable again\n");
		return -1;
	}

	sunxi_ehci->probe = 0;

	DMSG_INFO("[%s]: sunxi_usb_disable_ehci\n", sunxi_ehci->hci_name);

	sunxi_ehci_hcd_remove(sunxi_ehci->pdev);

	return 0;
}
EXPORT_SYMBOL(sunxi_usb_disable_ehci);

int sunxi_usb_enable_ehci(__u32 usbc_no)
{
	struct sunxi_hci_hcd *sunxi_ehci = NULL;

	sunxi_ehci = g_sunxi_ehci[usbc_no];
	if(sunxi_ehci == NULL){
		DMSG_PANIC("ERR: sunxi_ehci is null\n");
		return -1;
	}

#if !defined (CONFIG_ARCH_SUN8IW8) && !defined (CONFIG_ARCH_SUN8IW7) && !defined (CONFIG_USB_HCD_ENHANCE)
	if(sunxi_ehci->host_init_state){
		DMSG_PANIC("ERR: not support sunxi_usb_enable_ehci\n");
		return -1;
	}
#endif

	if(sunxi_ehci->probe == 1){
		DMSG_PANIC("ERR: sunxi_ehci is already enable, can not enable again\n");
		return -1;
	}

	sunxi_ehci->probe = 1;

	DMSG_INFO("[%s]: sunxi_usb_enable_ehci\n", sunxi_ehci->hci_name);

	sunxi_ehci_hcd_probe(sunxi_ehci->pdev);

	return 0;
}
EXPORT_SYMBOL(sunxi_usb_enable_ehci);

