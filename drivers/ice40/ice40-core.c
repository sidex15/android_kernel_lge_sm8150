/*
 * Base Driver for ice40 Accessory Communication Interface
 *
 * Copyright (c) 2018 LG Electronics, Inc
 *
 * All rights are reserved.
 *
 * IMPORTANT - PLEASE READ CAREFULLY BEFORE COPYING, INSTALLING OR USING
 * THE SOFTWARE.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *      PROJECT:   ice40 driver
 *      LANGUAGE:  ANSI C
 */

/* ========================================================================== */
/* Includes and Defines */
#ifndef DEBUG
#define DEBUG
#endif
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#ifdef OUT_OF_TREE
#include "ice40-core.h"
#else
#include <linux/ice40/ice40-core.h>
#endif
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/usb/usbpd.h>
#include <soc/qcom/lge/board_lge.h>

#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY) || IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
#include <linux/lge_cover_display.h>
#endif

#define DEBUG_GET_MCU_LOG

extern int lge_get_download_mode(void);
#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
extern void request_cover_recovery(int num);
#elif IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
extern void request_dualscreen_recovery(void);
#endif
bool is_cover_connection_state_connected(void);
struct ice40 *global_ice40;
static struct device *hiddenmenu_sysfs_dev;
static struct class *ice40_class;
/* ======================================================================== */
/* LOCAL FUNCTION PROTOTYPES */
static ssize_t ice40_show_enable(struct device *dev,
    struct device_attribute *attr, char *buf);
static ssize_t ice40_store_enable(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t size);
static ssize_t ice40_show_poll(struct device *dev,
    struct device_attribute *attr, char *buf);
static ssize_t ice40_store_poll(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t size);

/* ======================================================================== */
/* DEVICE ATTRIBUTES */
/* RW */
struct device_attribute ice40_attrs =
    __ATTR(FILE_ICE40, 0660, ice40_show_enable, ice40_store_enable);
struct device_attribute ice40_hiddenmenu_attrs =
    __ATTR(FILE_ICE40_POLL, 0660, ice40_show_poll, ice40_store_poll);

/*!
 *****************************************************************************
 *  \brief Disables ice40 Communication and enable bypass-mode
 *
 *  Disables the ice40 communication, therfore also both involved devices and
 *  clears the EN pin of AS3440 to enable the bypass-mode
 *
 *  \param ice40 : global driver data structure
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
int ice40_disable(struct ice40 *ice40)
{
    int ret;

    dev_dbg(ice40->dev, "%s()\n", __func__);

    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s: already disabled\n", __func__);
        return -EINVAL;
    }

    /* remove sysfs files */
    ret = mcu_i2c2_deinitialize_sysfs(ice40);
    if (ret < 0) {
        dev_info(ice40->dev, "%s: mcu_i2c2_deinitialize fail\n", __func__);
        return ret;
    }

    cancel_delayed_work(&ice40->status_monitor);

    ice40->ice40_on = false;
#ifdef DEBUG_GET_MCU_LOG
    ice40->get_log_count = 0;
#endif
    dev_info(ice40->dev, "%s: ice40 disabled\n", __func__);

    return ret;
}
EXPORT_SYMBOL(ice40_disable);

/*!
 *****************************************************************************
 *  \brief Enables ice40 Communication
 *
 *  Sets EN pin, initializes regmaps of both involved devices,
 *  checks device IDs, enables interrupts and sets-up all device interfaces
 *
 *  \param ice40 : global driver data structure
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
int ice40_enable(struct ice40 *ice40)
{
    int ret;

    dev_dbg(ice40->dev, "%s()\n", __func__);

    if (ice40->ice40_on) {
        dev_warn(ice40->dev, "%s: already enabled\n", __func__);
        return -EINVAL;
    }

    /*------------------------------------------------------- */
    /* initialize master regmap, sysfs, work-queues */
    ret = mcu_i2c2_initialize(ice40);
    if (ret < 0) {
        dev_info(ice40->dev, "%s: mcu_i2c2_initialize fail\n", __func__);
        return ret;
    }

    schedule_delayed_work(&ice40->status_monitor, usecs_to_jiffies(ICE40_STATUS_POLL_US));

    ice40->ice40_on = true;
#ifdef DEBUG_GET_MCU_LOG
    ice40->get_log_count = 0;
#endif
    dev_info(ice40->dev, "%s: ice40 enabled\n", __func__);
    return ret;
}
EXPORT_SYMBOL(ice40_enable);

static void ice40_mcu_recovery(void)
{
    if (COVER_DISPLAY_STATE_CONNECTED_POWERDROP != get_cover_display_state() && !global_ice40->in_recovery) {
        global_ice40->in_recovery = 1;
#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
        request_cover_recovery(1);
#elif IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
        request_dualscreen_recovery();
#endif
        global_ice40->recovery_count++;
    }
#ifdef CONFIG_LGE_HANDLE_PANIC
    if(global_ice40->recovery_count >= ICE40_RECOVREY_MAX_COUNT && !lge_get_factory_boot() && lge_get_download_mode())
        panic("ICE40_RECOVREY_MAX_COUNT EXCEEDED");
#endif
    printk(KERN_ERR "%s %d\n",__func__, global_ice40->recovery_count);
}

int ice40_master_reg_write(struct ice40 *ice40, uint addr, uint val)
{
    int ret;
/*
    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
*/
    ret = regmap_write(ice40->regmap_master, addr, val);

    if(ret < 0) {
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, err=%d\n", __func__, addr, val, ret);
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
        return ret;
    }
    dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr, val);

    return ret;
}
EXPORT_SYMBOL(ice40_master_reg_write);

int ice40_master_reg_read(struct ice40 *ice40, uint addr, uint *data)
{
    int ret;
/*
    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
*/
    ret = regmap_read(ice40->regmap_master, addr, data);
    if(ret < 0){
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, err=%d\n", __func__, addr, *data, ret);
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
        return ret;
    }
    dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr, *data);

    return ret;
}
EXPORT_SYMBOL(ice40_master_reg_read);

int ice40_master_bulk_read(struct ice40 *ice40, uint addr, char *data, int len)
{
    int ret;
    int i;
/*
    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
*/
    ret = regmap_bulk_read(ice40->regmap_master, addr, data, len);
    if(ret < 0){
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, err=%d\n", __func__, addr, *data, ret);
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
        return ret;
    }
    for(i = 0; i < len; i++) {
        dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr+i, data[i]);
    }

    return ret;
}
EXPORT_SYMBOL(ice40_master_bulk_read);

int ice40_mcu_reg_write(struct ice40 *ice40, uint addr, uint val)
{
    int ret;
    u8 data[3] = {0,};

    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
    mutex_lock(&ice40->mcu_i2c_lock);
    data[0] = ( addr >> 8 ) & 0xff;
    data[1] = addr & 0xff;
    data[2] = (u8)val;

    //ret = regmap_write(ice40->regmap_mcu, addr, val);
    ret = i2c_master_send(ice40->client_mcu, data, sizeof(data));

    if(ret < 0) {
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, err=%d\n", __func__, addr, val, ret);
        mutex_unlock(&ice40->mcu_i2c_lock);
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
        return ret;
    }
    ice40->recovery_count = 0;
    dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr, val);
    udelay(500);
    mutex_unlock(&ice40->mcu_i2c_lock);

    return ret;
}

int ice40_mcu_reg_write_norecovery(struct ice40 *ice40, uint addr, uint val)
{
    int ret;
    u8 data[3] = {0,};

    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
    mutex_lock(&ice40->mcu_i2c_lock);
    data[0] = ( addr >> 8 ) & 0xff;
    data[1] = addr & 0xff;
    data[2] = (u8)val;

    //ret = regmap_write(ice40->regmap_mcu, addr, val);
    ret = i2c_master_send(ice40->client_mcu, data, sizeof(data));

    if(ret < 0) {
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, err=%d\n", __func__, addr, val, ret);
        mutex_unlock(&ice40->mcu_i2c_lock);
        return ret;
    }
    dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr, val);
    udelay(500);
    mutex_unlock(&ice40->mcu_i2c_lock);

    return ret;
}

int ice40_mcu_reg_write_multi(struct ice40 *ice40, uint addr, u8 *val)
{
    int ret;
    u8 data[6] = {0,};

    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }
    mutex_lock(&ice40->mcu_i2c_lock);
    data[0] = ( addr >> 8 ) & 0xff;
    data[1] = addr & 0xff;
    data[2] = val[0];
    data[3] = val[1];
    data[4] = val[2];
    data[5] = val[3];

    ret = i2c_master_send(ice40->client_mcu, data, 6);

    if(ret < 0) {
        dev_err(ice40->dev, "%s FAIL[0x%x] = %x, %x, %x, %x, err=%d\n", __func__, addr, val[0], val[1], val[2], val[3], ret);
        mutex_unlock(&ice40->mcu_i2c_lock);
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
        return ret;
    }
    ice40->recovery_count = 0;
    dev_info(ice40->dev, "%s OK[0x%x] = %x, %x, %x, %x\n", __func__, addr, val[0], val[1], val[2], val[3]);
    udelay(500);
    mutex_unlock(&ice40->mcu_i2c_lock);

    return ret;
}

int ice40_mcu_reg_read(struct ice40 *ice40, uint addr, char *data, int len)
{
    int ret;
    u8 *buf;
    struct i2c_msg xfer[2];

    if (!ice40->ice40_on) {
        dev_warn(ice40->dev, "%s:  disabled\n", __func__);
        return -EINVAL;
    }

    mutex_lock(&ice40->mcu_i2c_lock);
    if( len < 2 )
        buf = kmalloc(2, GFP_KERNEL);
    else
        buf = kmalloc(len, GFP_KERNEL);

    if(!buf) {
        dev_err(ice40->dev, "%s: kmalloc error\n");
        mutex_unlock(&ice40->mcu_i2c_lock);
        return -ENOMEM;
    }

    buf[0] = ( addr >> 8 ) & 0xff;
    buf[1] = addr & 0xff;

    xfer[0].addr = ice40->client_mcu->addr;
    xfer[0].flags = 0;
    xfer[0].len = 2;
    xfer[0].buf = buf;

    xfer[1].addr = ice40->client_mcu->addr;
    xfer[1].flags = I2C_M_RD;
    xfer[1].len = len;
    //xfer[1].buf = data;
    xfer[1].buf = buf;

    //ret = regmap_read(ice40->regmap_mcu, addr, data);
    //ret = i2c_master_recv(ice40->client_mcu, buf, len );
    ret = i2c_transfer(ice40->client_mcu->adapter, xfer, 2);
    if(ret == 2){
        ret = 0;
        memcpy(data, buf, len);
        dev_info(ice40->dev, "%s OK[0x%x] = %x\n", __func__, addr, buf[0]);
        ice40->recovery_count = 0;
    } else {
        if (ret >= 0)
            ret = -EIO;
        dev_err(ice40->dev, "%s FAIL[0x%x], err=%d\n", __func__, addr, ret);
    }
    kfree(buf);
    udelay(500);
    mutex_unlock(&ice40->mcu_i2c_lock);

    if(ret < 0) {
        if( ret != -ENOTCONN && ret != -EACCES )
            ice40_mcu_recovery();
    }

    return ret;
}
EXPORT_SYMBOL(ice40_mcu_reg_read);

static void ice40_check_status(struct ice40 *ice40)
{
    int ret;
    char udata[3] = {0,};
#ifdef DEBUG_GET_MCU_LOG
    int i;
    char data[80] = {0,};
#else
    u8 data = 0;
#endif

    ret = ice40_master_bulk_read(ice40, 0x00, udata, sizeof(udata));
    if(ret < 0) {
        dev_info(ice40->dev, "%s %d\n", __func__, ret);
    }
    if( (udata[0] & 0x40) && ~(udata[0] & 0x1) ) {      //check onewire link and DP path.
#ifdef DEBUG_GET_MCU_LOG
        if(ice40->get_log_count >= ICE40_GET_MCU_LOG_COUNT) {
            ret = ice40_mcu_reg_read(ice40, I2C_R_MCU_LOG, data, sizeof(data));
            if(ret >= 0) {
                dev_info(ice40->dev, "%s MCU LOG : %s\n", __func__, &data[1]); //data[0] is \n
            }
        } else {
            for(i = 0; i < ICE40_GET_MCU_LOG_COUNT; i++) {
                ret = ice40_mcu_reg_read(ice40, I2C_R_MCU_LOG, data, sizeof(data));
                if(ret >= 0) {
                    dev_info(ice40->dev, "%s MCU LOG : %s\n", __func__, &data[1]); //data[0] is \n
                    ice40->get_log_count++;
                    if(ice40->get_log_count == ICE40_GET_MCU_LOG_COUNT) break;
                }
            }
        }
#else
        ret = ice40_mcu_reg_read(ice40, I2C_R_DISPLAY_ID, (char *)&data, sizeof(data));
        if(ret >= 0) {
            dev_info(ice40->dev, "%s %x\n", __func__, data);
        }
#endif
    }
}

static void ice40_poll_loop(struct work_struct *work)
{
    struct ice40 *ice40 = container_of(to_delayed_work(work), struct ice40, status_monitor);

    if(ice40->ice40_on) {
        ice40_check_status(ice40);
        schedule_delayed_work(&ice40->status_monitor, usecs_to_jiffies(ICE40_STATUS_POLL_US));
    }
}

/* ======================================================================== */
/* LOCAL FUNCTIONS */
/*!
 *****************************************************************************
 *  \brief Returns current ice40 status
 *
 *  Access through sysfs file "ice40"
 *
 *  \return xx: returns number of bytes written
 *
 *****************************************************************************
 */
static ssize_t ice40_show_enable(struct device *dev,
    struct device_attribute *attr, char *buf)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);

    dev_dbg(ice40->dev, "%s()\n", __func__);

    return snprintf(buf, PAGE_SIZE, "%d", ice40->ice40_on);
}

/*!
 *****************************************************************************
 *  \brief Enables or disables ice40
 *
 *  Access through sysfs file "ice40"
 *  calls ice40_enable or ice40_disable
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
static ssize_t ice40_store_enable(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t size)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);
    int value;
    int ret;

    dev_info(ice40->dev, "%s()\n", __func__);

    ret = kstrtoint(buf, 10, &value);
    if (ret < 0) {
        dev_err(ice40->dev, "sscanf error %d\n", ret);
        return ret;
    }

    if (value) {
        ret = ice40_enable(ice40);
        if (ret < 0) {
            dev_info(ice40->dev, "%s ice40_enable fail\n", __func__);
            return ret;
        }
    } else {
        ret = ice40_disable(ice40);
        if (ret < 0) {
            dev_info(ice40->dev, "%s ice40_disable fail\n", __func__);
            return ret;
        }
    }
    return size;
}

/*!
 *****************************************************************************
 *  \brief Returns poll status
 *
 *  Access through sysfs file "ice40"
 *
 *  \return xx: returns number of bytes written
 *
 *****************************************************************************
 */
static ssize_t ice40_show_poll(struct device *dev,
    struct device_attribute *attr, char *buf)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);
    dev_dbg(ice40->dev, "%s()\n", __func__);
    return 0;
}

/*!
 *****************************************************************************
 *  \brief cancel monitor work
 *
 *  Access through sysfs file "ice40"
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
static ssize_t ice40_store_poll(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t size)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);
    int value;
    int ret;

    dev_info(ice40->dev, "%s()\n", __func__);

    ret = kstrtoint(buf, 10, &value);
    if (ret < 0) {
        dev_err(ice40->dev, "sscanf error %d\n", ret);
        return ret;
    }
    if(value)
        cancel_delayed_work(&ice40->status_monitor);

    return size;
}

/*!
 *****************************************************************************
 *  \brief Called after suspend event
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
static int ice40_suspend(struct device *dev)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);

    /* FIXME: what should we do here? */
    if (ice40->ice40_on) {
         cancel_delayed_work(&ice40->status_monitor);
    }

    return 0;
}

/*!
 *****************************************************************************
 *  \brief Called after resume event
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
static int ice40_resume(struct device *dev)
{
    struct ice40 *ice40 = dev_get_drvdata(dev);

    /* FIXME: what should we do here? */
    if (ice40->ice40_on) {
         schedule_delayed_work(&ice40->status_monitor, usecs_to_jiffies(ICE40_STATUS_POLL_US));
    }

    return 0;
}

/* register new I2C device for MCU_I2C2 */
static struct i2c_board_info mcu_i2c2_info = {
    I2C_BOARD_INFO("mcu_i2c2", 0x5c),
};

/* Instantiate the AS3445 I2C device */
static struct i2c_client *mcu_i2c2_init_i2c_dev(struct i2c_adapter *adapter)
{
    dev_dbg(&adapter->dev, "Instantiating device %s at 0x%02x\n",
            mcu_i2c2_info.type, mcu_i2c2_info.addr);
    return i2c_new_device(adapter, &mcu_i2c2_info);
}
/* ========================================================================== */
/* Probe, Remove functions */


/*!
 *****************************************************************************
 *  \brief Probe function
 *
 *  Allocates memory for ice40 device, configures GPIO pins,
 *  configures IRQ routine, configures input events for buttons
 *
 *  \return xx: returns 0 on success
 *
 *****************************************************************************
 */
static int ice40_i2c_probe(struct i2c_client *client,
    const struct i2c_device_id *id)
{
    struct ice40 *ice40;
    int ret;

    /* info: memory allocated with devm_kzalloc is automatically freed
     * upon driver detach, difference between kzalloc() and devm_kzalloc()
     */
    ice40 = devm_kzalloc(&client->dev, sizeof(struct ice40), GFP_KERNEL);
    if (!ice40)
        return -ENOMEM;
    global_ice40 = ice40;

    ice40->client_master = client;
    ice40->dev = &client->dev;
    i2c_set_clientdata(client, ice40);
    mutex_init(&ice40->mcu_i2c_lock);

    dev_dbg(ice40->dev, "%s()\n", __func__);
    INIT_DELAYED_WORK(&ice40->status_monitor, ice40_poll_loop);

    ice40->client_mcu = mcu_i2c2_init_i2c_dev(client->adapter);
    if (ice40->client_mcu == NULL) {
        dev_err(&client->dev, "%s: could not init mcu i2c device\n",
                __func__);
        ret = -EIO;
        goto exit_kfree;
    }
    i2c_set_clientdata(ice40->client_mcu, ice40);

    ice40->ice40_on = false;

    /*--------------------------------------------------------------- */
    /* sysfs file creation */
    ret = ice40m_initialize(ice40);
    if (ret < 0) {
        dev_info(ice40->dev, "%s: ice40m_initialize fail\n", __func__);
        goto dev_attr_failed1;
    }

    ret = device_create_file(&client->dev, &ice40_attrs);
    if (ret) {
        dev_err(&client->dev, "%s: could not create sysfs attribute file\n",
                __func__);
        goto dev_attr_failed2;
    }
    ice40_class = class_create(THIS_MODULE, "ice40_i2c");
    hiddenmenu_sysfs_dev = device_create(ice40_class, NULL, 0, ice40, "coverdisplay");
    ret = device_create_file(hiddenmenu_sysfs_dev, &ice40_hiddenmenu_attrs);
     if (ret) {
        dev_err(&client->dev, "%s: could not create hiddenmenu sysfs attribute file\n",
                __func__);
        goto dev_attr_failed3;
    }

    dev_info(&client->dev, "%s: successfully configured\n", __func__);

    return 0;

dev_attr_failed3:
    device_remove_file(hiddenmenu_sysfs_dev, &ice40_hiddenmenu_attrs);
    class_destroy(ice40_class);
dev_attr_failed2:
    device_remove_file(&client->dev, &ice40_attrs);
dev_attr_failed1:
    ice40m_deinitialize_sysfs(ice40);
    i2c_set_clientdata(ice40->client_mcu, NULL);
    i2c_unregister_device(ice40->client_mcu);
exit_kfree:
    i2c_set_clientdata(client, NULL);
    devm_kfree(&client->dev, ice40);
    return ret;
}
EXPORT_SYMBOL(global_ice40);
/*!
 *****************************************************************************
 *  \brief Driver remove function
 *
 *  Removes sysfs files, frees interrupt, deallocates memory
 *
 *  \return xx: returns 0
 *
 *****************************************************************************
 */
static int ice40_i2c_remove(struct i2c_client *client)
{
    struct ice40 *ice40 = i2c_get_clientdata(client);

    dev_dbg(&client->dev, "%s()\n", __func__);

    device_remove_file(&client->dev, &ice40_attrs);
    device_remove_file(hiddenmenu_sysfs_dev, &ice40_hiddenmenu_attrs);
    class_destroy(ice40_class);

    if (ice40) {
        ice40_disable(ice40);
        /* deinit interrupts and cancel master work queue */
        ice40m_deinitialize(ice40);

        /* cancel slave work queue */
        mcu_i2c2_deinitialize(ice40);

        if (ice40->client_mcu) {
            i2c_set_clientdata(ice40->client_mcu, NULL);
            i2c_unregister_device(ice40->client_mcu);
        }

        i2c_set_clientdata(client, NULL);
        devm_kfree(&client->dev, ice40);
    }

    return 0;
}

/* ========================================================================== */
/* Driver Matching */

#ifdef CONFIG_OF
/* find a match from full device tree entries (including vendor part) */
static const struct of_device_id ice40_of_match[] = {
    {.compatible = "lattice,ice40-i2c",},
    {}
};
MODULE_DEVICE_TABLE(of, ice40_of_match);
#endif

/* find a match in device tree entries */
static const struct i2c_device_id ice40_i2c_id[] = {
    {.name = "ice40_i2c", .driver_data = 0},
    {}
};
MODULE_DEVICE_TABLE(i2c, ice40_i2c_id);

/* ========================================================================== */
/* Driver Definition */

static const struct dev_pm_ops ice40_pm_ops = {
    .suspend = ice40_suspend,
    .resume  = ice40_resume,
};

static struct i2c_driver ice40_i2c_driver = {
    .driver = {
       .name = "ice40_i2c",
       .owner = THIS_MODULE,
       .of_match_table = ice40_of_match,
       .pm = &ice40_pm_ops,
    },
    .probe = ice40_i2c_probe,
    .remove = ice40_i2c_remove,
    .id_table = ice40_i2c_id,
};

//module_i2c_driver(ice40_i2c_driver);

static int __init ice40_i2c_init(void)
{
    int ret = 0;

    pr_info("ice40 driver  \n");

    ret = i2c_add_driver(&ice40_i2c_driver);
    pr_info("ice40 init %d \n",ret);
    return ret;
}
module_init(ice40_i2c_init);


static void __exit ice40_i2c_exit(void)
{
    i2c_del_driver(&ice40_i2c_driver);

}
module_exit(ice40_i2c_exit);


MODULE_AUTHOR("LG Electronics, Inc");
MODULE_DESCRIPTION("ICE40 core driver");
MODULE_LICENSE("GPL v2");
