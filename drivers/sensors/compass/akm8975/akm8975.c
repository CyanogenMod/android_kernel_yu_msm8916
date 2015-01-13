/*
 *  akm8975.c - Definitions for akm8975 compass chip
 *
 *  Copyright (C) 2010 Yulong Tech. Co., Ltd.
 *  Jay.HF <huangfujie@yulong.com>
 *  Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <linux/module.h>
#include <linux/sensors/akm8975.h>
#include <linux/regulator/consumer.h>

#define AKM8975_PWROFF_EN     0 /* macro of sensor power off enable */

#define AKM8975_DEBUG         0
#define AKM8975_DEBUG_MSG     0
#define AKM8975_DEBUG_FUNC    0
#define AKM8975_DEBUG_DATA    0
#define MAX_FAILURE_COUNT     3
#define AKM8975_RETRY_COUNT   10
#define AKM8975_DEFAULT_DELAY 100000000

#define AKM_ACCEL_ITEMS       3
#define AKM_ACCEL_X           0
#define AKM_ACCEL_Y           1
#define AKM_ACCEL_Z           2

#if AKM8975_DEBUG_MSG
#define AKMDBG(format, ...) \
        printk(KERN_INFO "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 " format "\n", ## __VA_ARGS__)
#else
#define AKMDBG(format, ...)
#endif

#if AKM8975_DEBUG_FUNC
#define AKMFUNC(func) \
        printk(KERN_INFO "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 " func " is called\n")
#else
#define AKMFUNC(func)
#endif

static struct i2c_client *this_client;

struct akm8975_data {
    struct i2c_client *client;
    struct input_dev *input_dev;
    struct work_struct work;

#ifdef CONFIG_HAS_EARLYSUSPEND
    struct early_suspend akm_early_suspend;
#endif
    /* add sensor regulators, modify Jay.HF 2012-12-18 */
    struct regulator *vdd;
    struct regulator *vcc_i2c;
    /* end */
};

/* Addresses to scan -- protected by sense_data_mutex */
static char sense_data[SENSOR_DATA_SIZE];
static struct mutex sense_data_mutex;
static DECLARE_WAIT_QUEUE_HEAD(data_ready_wq);
static DECLARE_WAIT_QUEUE_HEAD(open_wq);

static atomic_t data_ready;
static atomic_t open_count;
static atomic_t open_flag;
static atomic_t reserve_open_flag;

static atomic_t m_flag;
static atomic_t a_flag;
static atomic_t mv_flag;

static int failure_count;

static int64_t akmd_delay[3] = {-1, -1, -1};
static int16_t akmd_accel[AKM_ACCEL_ITEMS] = {0, 0, 720};
static atomic_t suspend_flag = ATOMIC_INIT(0);

static struct akm8975_platform_data *pdata;

static int been_actived = 0;

#ifdef CONFIG_PM
static int akm8975_suspend(struct device *dev);
static int akm8975_resume(struct device *dev);
#endif

static int akm8975_power_on(struct akm8975_data *data, bool on)
{
    int rc;

    if (!on)
        goto power_off;

    rc = regulator_enable(data->vdd);
    if (rc) {
        dev_err(&data->client->dev,
            "Regulator vdd enable failed rc=%d\n", rc);
        return rc;
    }

    rc = regulator_enable(data->vcc_i2c);
    if (rc) {
        dev_err(&data->client->dev,
            "Regulator vcc_i2c enable failed rc=%d\n", rc);
        regulator_disable(data->vdd);
    }

    return rc;

power_off:
    rc = regulator_disable(data->vdd);
    if (rc) {
        dev_err(&data->client->dev,
            "Regulator vdd disable failed rc=%d\n", rc);
        return rc;
    }

    rc = regulator_disable(data->vcc_i2c);
    if (rc) {
        dev_err(&data->client->dev,
            "Regulator vcc_i2c disable failed rc=%d\n", rc);
        regulator_enable(data->vdd);
    }

    return rc;
}

static int akm8975_power_init(struct akm8975_data *data, bool on)
{
    int rc;

    if (!on)
        goto pwr_deinit;

    data->vdd = regulator_get(&data->client->dev, "vdd_ana");
    if (IS_ERR(data->vdd)) {
        rc = PTR_ERR(data->vdd);
        dev_err(&data->client->dev,
            "Regulator get failed vdd rc=%d\n", rc);
        return rc;
    }

    if (regulator_count_voltages(data->vdd) > 0) {
        rc = regulator_set_voltage(data->vdd, 2600000,3300000);
        if (rc) {
            dev_err(&data->client->dev,
                "Regulator set_vtg failed vdd rc=%d\n", rc);
            goto reg_vdd_put;
        }
    }

    data->vcc_i2c = regulator_get(&data->client->dev, "vcc_i2c");
    if (IS_ERR(data->vcc_i2c)) {
        rc = PTR_ERR(data->vcc_i2c);
        dev_err(&data->client->dev,
            "Regulator get failed vcc_i2c rc=%d\n", rc);
        goto reg_vdd_set_vtg;
    }

    if (regulator_count_voltages(data->vcc_i2c) > 0) {
        rc = regulator_set_voltage(data->vcc_i2c, 1800000,1800000);
        if (rc) {
            dev_err(&data->client->dev,
            "Regulator set_vtg failed vcc_i2c rc=%d\n", rc);
            goto reg_vcc_i2c_put;
        }
    }

    return 0;

reg_vcc_i2c_put:
    regulator_put(data->vcc_i2c);
reg_vdd_set_vtg:
    if (regulator_count_voltages(data->vdd) > 0)
        regulator_set_voltage(data->vdd, 0, 3300000);
reg_vdd_put:
    regulator_put(data->vdd);
    return rc;

pwr_deinit:
    if (regulator_count_voltages(data->vdd) > 0)
        regulator_set_voltage(data->vdd, 0, 3300000);

    regulator_put(data->vdd);

    if (regulator_count_voltages(data->vcc_i2c) > 0)
        regulator_set_voltage(data->vcc_i2c, 0, 1800000);

    regulator_put(data->vcc_i2c);
    return 0;
}

static int AKI2C_RxData(char *rxData, int length)
{
    uint8_t loop_i;
    struct i2c_msg msgs[] = {
        {
            .addr = this_client->addr,
            .flags = 0,
            .len = 1,
            .buf = rxData,
        },
        {
            .addr = this_client->addr,
            .flags = I2C_M_RD,
            .len = length,
            .buf = rxData,
        },
    };
#if AKM8975_DEBUG_DATA
    char addr = rxData[0];
#endif

#ifdef AKM8975_DEBUG
    /* Caller should check parameter validity.*/
    if ((rxData == NULL) || (length < 1)) {
        return -EINVAL;
    }
#endif
    for (loop_i = 0; loop_i < AKM8975_RETRY_COUNT; loop_i++) {
        if (i2c_transfer(this_client->adapter, msgs, 2) > 0) {
            break;
        }
        mdelay(10);
    }

    if (loop_i >= AKM8975_RETRY_COUNT) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s retry over %d\n",
                __func__, AKM8975_RETRY_COUNT);
        return -EIO;
    }
#if AKM8975_DEBUG_DATA
    printk(KERN_INFO "BJ_BSP_DRIVER: CP_COMPASS: RxData: len=%02x, addr=%02x", length, addr);
    printk(KERN_INFO "BJ_BSP_DRIVER: CP_COMPASS: data=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
           rxData[0],rxData[1],rxData[2],rxData[3],
           rxData[4],rxData[5],rxData[6],rxData[7]);
#endif
    return 0;
}

static int AKI2C_TxData(char *txData, int length)
{
    uint8_t loop_i = 0;

    struct i2c_msg msg[] = {
        {
            .addr = this_client->addr,
            .flags = 0,
            .len = length,
            .buf = txData,
        },
    };
#if AKM8975_DEBUG_DATA
    int i;
#endif

#ifdef AKM8975_DEBUG
    /* Caller should check parameter validity.*/
    if ((txData == NULL) || (length < 2)) {
        return -EINVAL;
    }
#endif
    for (loop_i = 0; loop_i < AKM8975_RETRY_COUNT; loop_i++) {
        if (i2c_transfer(this_client->adapter, msg, 1) > 0) {
            break;
        }
        mdelay(10);
    }

    if (loop_i >= AKM8975_RETRY_COUNT) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: %s retry over %d\n",
                __func__, AKM8975_RETRY_COUNT);
        return -EIO;
    }
#if AKM8975_DEBUG_DATA
    printk(KERN_INFO "BJ_BSP_DRIVER: CP_COMPASS: TxData: len=%02x, addr=%02x\n  data=",
            length, txData[0]);
    for (i = 0; i < (length-1); i++) {
        printk(KERN_INFO " %02x", txData[i + 1]);
    }
    printk(KERN_INFO "\n");
#endif
    return 0;
}

static int AKECS_SetMode_SngMeasure(void)
{
    char buffer[2];
    atomic_set(&data_ready, 0);
    /* Set measure mode */
    buffer[0] = AK8975_REG_CNTL;
    buffer[1] = AK8975_MODE_SNG_MEASURE;

    /* Set data */
    return AKI2C_TxData(buffer, 2);
}

static int AKECS_SetMode_SelfTest(void)
{
    char buffer[2];
    /* Set measure mode */
    buffer[0] = AK8975_REG_CNTL;
    buffer[1] = AK8975_MODE_SELF_TEST;
    /* Set data */
    return AKI2C_TxData(buffer, 2);
}

static int AKECS_SetMode_FUSEAccess(void)
{
    char buffer[2];
    /* Set measure mode */
    buffer[0] = AK8975_REG_CNTL;
    buffer[1] = AK8975_MODE_FUSE_ACCESS;
    /* Set data */
    return AKI2C_TxData(buffer, 2);
}

static int AKECS_SetMode_PowerDown(void)
{
    char buffer[2];
    /* Set powerdown mode */
    buffer[0] = AK8975_REG_CNTL;
    buffer[1] = AK8975_MODE_POWERDOWN;
    /* Set data */
    return AKI2C_TxData(buffer, 2);
}

static int AKECS_SetMode(char mode)
{
    int ret = 0;
    switch (mode) {
    case AK8975_MODE_SNG_MEASURE:
        ret = AKECS_SetMode_SngMeasure();
        break;
    case AK8975_MODE_SELF_TEST:
        ret = AKECS_SetMode_SelfTest();
        break;
    case AK8975_MODE_FUSE_ACCESS:
        ret = AKECS_SetMode_FUSEAccess();
        break;
    case AK8975_MODE_POWERDOWN:
        ret = AKECS_SetMode_PowerDown();
        /* wait at least 100us after changing mode */
        udelay(100);
        break;
    default:
        AKMDBG("%s: Unknown mode(%d)", __func__, mode);
        return -EINVAL;
    }

    return ret;
}

static int AKECS_CheckDevice(void)
{
    char buffer[2];
    int ret = 0;
    /* Set measure mode */
    buffer[0] = AK8975_REG_WIA;
    /* Read data */
    ret = AKI2C_RxData(buffer, 1);

    if (ret < 0) {
        return ret;
    }
    /* Check read data */
    if (buffer[0] != 0x48) {
        return -ENXIO;
    }

    return 0;
}

static int AKECS_GetData(char *rbuf, int size)
{
#ifdef AKM8975_DEBUG
    /* This function is not exposed, so parameters
       should be checked internally.*/
    if ((rbuf == NULL) || (size < SENSOR_DATA_SIZE)) {
        return -EINVAL;

    }
#endif
    wait_event_interruptible_timeout(
        data_ready_wq, atomic_read(&data_ready), 1000);
    if (!atomic_read(&data_ready)) {
        AKMDBG("%s: data_ready is not set.", __func__);
        if (!atomic_read(&suspend_flag)) {
            AKMDBG("%s: suspend_flag is not set.", __func__);
            failure_count++;
            if (failure_count >= MAX_FAILURE_COUNT) {
                printk(KERN_ERR
                    "AKM8975 AKECS_GetData: "
                    "successive %d failure.\n",
                    failure_count);
                atomic_set(&open_flag, -1);
                wake_up(&open_wq);
                failure_count = 0;
            }
        }
        return -1;
    }

    mutex_lock(&sense_data_mutex);
    memcpy(rbuf, sense_data, size);
    atomic_set(&data_ready, 0);
    mutex_unlock(&sense_data_mutex);

    failure_count = 0;
    return 0;
}

static void AKECS_SetYPR(short *rbuf)
{
    struct akm8975_data *data = i2c_get_clientdata(this_client);
#if 0
    printk(KERN_INFO "AKM8975 %s: flag =0x%X\n", __func__, rbuf[0]);
    printk(KERN_INFO "  Geomagnetism[LSB]: %6d,%6d,%6d stat=%d\n",
           rbuf[1], rbuf[2], rbuf[3], rbuf[4]);
    printk(KERN_INFO "  Acceleration[LSB]: %6d,%6d,%6d stat=%d\n",
           rbuf[5], rbuf[6], rbuf[7], rbuf[8]);
    printk(KERN_INFO "  yaw =%6d, pitch =%6d, roll =%6d\n",
           rbuf[9], rbuf[10], rbuf[11]);
#endif
    /* Report magnetic vector information */
    if (atomic_read(&mv_flag) && (rbuf[0] & MAG_DATA_READY)) {
        //printk(KERN_INFO"%s: magnetic data  \n", __func__);
        input_report_abs(data->input_dev, ABS_HAT0X, rbuf[1]);
        input_report_abs(data->input_dev, ABS_HAT0Y, rbuf[2]);
        input_report_abs(data->input_dev, ABS_BRAKE, rbuf[3]);
        input_report_abs(data->input_dev, ABS_GAS, rbuf[4]);
    }
    /* Report acceleration sensor information */
    if (atomic_read(&a_flag) && (rbuf[0] & ACC_DATA_READY)) {
        //printk(KERN_INFO"%s: acceleration data   \n", __func__);
        input_report_abs(data->input_dev, ABS_X, rbuf[5]);
        input_report_abs(data->input_dev, ABS_Y, rbuf[6]);
        input_report_abs(data->input_dev, ABS_Z, rbuf[7]);
        input_report_abs(data->input_dev, ABS_WHEEL, rbuf[8]);
    }
    /* Report orientation sensor information */
    if (atomic_read(&m_flag) && (rbuf[0] & ORI_DATA_READY)) {
        //printk(KERN_INFO"%s: orientation data  \n", __func__);
        input_report_abs(data->input_dev, ABS_RX, rbuf[9]);
        input_report_abs(data->input_dev, ABS_RY, rbuf[10]);
        input_report_abs(data->input_dev, ABS_RZ, rbuf[11]);
        input_report_abs(data->input_dev, ABS_RUDDER, rbuf[4]);
    }

    if (rbuf[0] != 0) {
        input_sync(data->input_dev);
    }
}

static int AKECS_GetOpenStatus(void)
{
    wait_event_interruptible(open_wq, (atomic_read(&open_flag) != 0));
    return atomic_read(&open_flag);
}

static int AKECS_GetCloseStatus(void)
{
    wait_event_interruptible(open_wq, (atomic_read(&open_flag) <= 0));
    return atomic_read(&open_flag);
}

static void AKECS_CloseDone(void)
{
    atomic_set(&m_flag, 0);
    atomic_set(&a_flag, 0);
    atomic_set(&mv_flag, 0);
}

/***** akm_aot functions ***************************************/
static int akm_aot_open(struct inode *inode, struct file *file)
{
    int ret = -1;
    AKMFUNC("akm_aot_open");
    if (atomic_cmpxchg(&open_count, 0, 1) == 0) {
        if (atomic_cmpxchg(&open_flag, 0, 1) == 0) {
            atomic_set(&reserve_open_flag, 1);
            wake_up(&open_wq);
            ret = 0;
        }
    }
    return ret;
}

static int akm_aot_release(struct inode *inode, struct file *file)
{
    AKMFUNC("akm_aot_release");
    atomic_set(&reserve_open_flag, 0);
    atomic_set(&open_flag, 0);
    atomic_set(&open_count, 0);
    wake_up(&open_wq);
    return 0;
}

static long akm_aot_ioctl(struct file *file,unsigned int cmd, unsigned long arg)
{
#if AKM8975_PWROFF_EN
    struct akm8975_data *akm = i2c_get_clientdata(this_client);
#endif
    void __user *argp = (void __user *)arg;
    short flag = 0;
    int64_t delay[3];
    int16_t accel[AKM_ACCEL_ITEMS];
    // printk(KERN_INFO"%s: ++  \r\n", __func__);

    switch (cmd) {
    case ECS_IOCTL_APP_SET_MFLAG:
    case ECS_IOCTL_APP_SET_AFLAG:
    case ECS_IOCTL_APP_SET_MVFLAG:
        if (copy_from_user(&flag, argp, sizeof(flag))) {
            return -EFAULT;
        }
        if (flag < 0 || flag > 1) {
            return -EINVAL;
        }
    #if AKM8975_PWROFF_EN
        /* zhangzhe: change power manage policy */
        if(flag && !been_actived) {
            if(akm8975_power_on(akm, true)) {
                pr_info(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s akm8975_power_on fail \r\n", __func__);
            }
        }
    #endif
        been_actived = 1;
        break;

    case ECS_IOCTL_APP_SET_DELAY:
        if (copy_from_user(&delay, argp, sizeof(delay))) {
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_APP_SET_ACCEL:
        if (copy_from_user(&accel, argp, sizeof(accel))) {
            return -EFAULT;
        }
        break;
    default:
        break;
    }

    switch (cmd) {
    case ECS_IOCTL_APP_SET_MFLAG:
        atomic_set(&m_flag, flag);
        AKMDBG("MFLAG is set to %d", flag);
        break;
    case ECS_IOCTL_APP_GET_MFLAG:
        flag = atomic_read(&m_flag);
        break;
    case ECS_IOCTL_APP_SET_AFLAG:
        atomic_set(&a_flag, flag);
        AKMDBG("AFLAG is set to %d", flag);
        break;
    case ECS_IOCTL_APP_GET_AFLAG:
        flag = atomic_read(&a_flag);
        break;
    case ECS_IOCTL_APP_SET_MVFLAG:
        atomic_set(&mv_flag, flag);
        AKMDBG("MVFLAG is set to %d", flag);
        break;
    case ECS_IOCTL_APP_GET_MVFLAG:
        flag = atomic_read(&mv_flag);
        break;
    case ECS_IOCTL_APP_SET_DELAY:
        akmd_delay[0] = delay[0];
        akmd_delay[1] = delay[1];
        akmd_delay[2] = delay[2];
        AKMDBG("Delay is set to %lld,%lld,%lld",
                akmd_delay[0],akmd_delay[1],akmd_delay[2]);
        break;
    case ECS_IOCTL_APP_GET_DELAY:
        delay[0] = akmd_delay[0];
        delay[1] = akmd_delay[1];
        delay[2] = akmd_delay[2];
        break;
    case ECS_IOCTL_APP_SET_ACCEL:
        akmd_accel[AKM_ACCEL_X] = accel[AKM_ACCEL_X];
        akmd_accel[AKM_ACCEL_Y] = accel[AKM_ACCEL_Y];
        akmd_accel[AKM_ACCEL_Z] = accel[AKM_ACCEL_Z];
        break;
    default:
        return -ENOTTY;
    }

    switch (cmd) {
    case ECS_IOCTL_APP_GET_MFLAG:
    case ECS_IOCTL_APP_GET_AFLAG:
    case ECS_IOCTL_APP_GET_MVFLAG:
        if (copy_to_user(argp, &flag, sizeof(flag))) {
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_APP_GET_DELAY:
        if (copy_to_user(argp, &delay, sizeof(delay))) {
            return -EFAULT;
        }
        break;
    default:
        break;
    }
    // printk(KERN_INFO"%s: -- \r\n", __func__);
    return 0;
}

/***** akmd functions ********************************************/
static int akmd_open(struct inode *inode, struct file *file)
{
    AKMFUNC("akmd_open");
    mdelay(10);
    return nonseekable_open(inode, file);
}

static int akmd_release(struct inode *inode, struct file *file)
{
    AKMFUNC("akmd_release");
    AKECS_CloseDone();
    return 0;
}

static long akmd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;

    /* NOTE: In this function the size of "char" should be 1-byte. */
    char sData[SENSOR_DATA_SIZE]; /* for GETDATA */
    char rwbuf[RWBUF_SIZE];       /* for READ/WRITE */
    char mode = 0;                /* for SET_MODE*/
    short value[12];              /* for SET_YPR */
    int64_t delay[3];             /* for GET_DELAY */
    int status = 0;               /* for OPEN/CLOSE_STATUS */
    int ret = -1;                 /* Return value. */
    int16_t accel[AKM_ACCEL_ITEMS];
    // AKMDBG("%s (0x%08X).", __func__, cmd);
    // printk(KERN_INFO"%s: +++ \r\n", __func__);

    switch (cmd) {
    case ECS_IOCTL_WRITE:
    case ECS_IOCTL_READ:
        if (argp == NULL) {
            AKMDBG("invalid argument.");
            return -EINVAL;
        }
        if (copy_from_user(&rwbuf, argp, sizeof(rwbuf))) {
            AKMDBG("copy_from_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_SET_MODE:
        if (argp == NULL) {
            AKMDBG("invalid argument.");
            return -EINVAL;
        }
        if (copy_from_user(&mode, argp, sizeof(mode))) {
            AKMDBG("copy_from_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_SET_YPR:
        if (argp == NULL) {
            AKMDBG("invalid argument.");
            return -EINVAL;
        }
        if (copy_from_user(&value, argp, sizeof(value))) {
            AKMDBG("copy_from_user failed.");
            return -EFAULT;
        }
        break;
    default:
        break;
    }

    switch (cmd) {
    case ECS_IOCTL_WRITE:
        AKMFUNC("IOCTL_WRITE");
        if ((rwbuf[0] < 2) || (rwbuf[0] > (RWBUF_SIZE-1))) {
            AKMDBG("invalid argument.");
            return -EINVAL;
        }
        ret = AKI2C_TxData(&rwbuf[1], rwbuf[0]);
        if (ret < 0) {
            return ret;
        }
        break;
    case ECS_IOCTL_READ:
        AKMFUNC("IOCTL_READ");
        if ((rwbuf[0] < 1) || (rwbuf[0] > (RWBUF_SIZE-1))) {
            AKMDBG("invalid argument.");
            return -EINVAL;
        }
        ret = AKI2C_RxData(&rwbuf[1], rwbuf[0]);
        if (ret < 0) {
            return ret;
        }
        break;
    case ECS_IOCTL_SET_MODE:
        AKMFUNC("IOCTL_SET_MODE");
        ret = AKECS_SetMode(mode);
        if (ret < 0) {
            return ret;
        }
        break;
    case ECS_IOCTL_GETDATA:
        AKMFUNC("IOCTL_GET_DATA");
        ret = AKECS_GetData(sData, SENSOR_DATA_SIZE);
        if (ret < 0) {
            return ret;
        }
        break;
    case ECS_IOCTL_SET_YPR:
        AKECS_SetYPR(value);
        break;
    case ECS_IOCTL_GET_OPEN_STATUS:
        AKMFUNC("IOCTL_GET_OPEN_STATUS");
        status = AKECS_GetOpenStatus();
        AKMDBG("AKECS_GetOpenStatus returned (%d)", status);
        break;
    case ECS_IOCTL_GET_CLOSE_STATUS:
        AKMFUNC("IOCTL_GET_CLOSE_STATUS");
        status = AKECS_GetCloseStatus();
        AKMDBG("AKECS_GetCloseStatus returned (%d)", status);
        break;
    case ECS_IOCTL_GET_DELAY:
        AKMFUNC("IOCTL_GET_DELAY");
        delay[0] = akmd_delay[0];
        delay[1] = akmd_delay[1];
        delay[2] = akmd_delay[2];
        break;
    case ECS_IOCTL_GET_ACCEL:
        AKMFUNC("IOCTL_GET_ACCEL");
        accel[AKM_ACCEL_X] = akmd_accel[AKM_ACCEL_X];
        accel[AKM_ACCEL_Y] = akmd_accel[AKM_ACCEL_Y];
        accel[AKM_ACCEL_Z] = akmd_accel[AKM_ACCEL_Z];
        break;
    default:
        return -ENOTTY;
    }

    switch (cmd) {
    case ECS_IOCTL_READ:
        if (copy_to_user(argp, &rwbuf, rwbuf[0]+1)) {
            AKMDBG("copy_to_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_GETDATA:
        if (copy_to_user(argp, &sData, sizeof(sData))) {
            AKMDBG("copy_to_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_GET_OPEN_STATUS:
    case ECS_IOCTL_GET_CLOSE_STATUS:
        if (copy_to_user(argp, &status, sizeof(status))) {
            AKMDBG("copy_to_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_GET_DELAY:
        if (copy_to_user(argp, &delay, sizeof(delay))) {
            AKMDBG("copy_to_user failed.");
            return -EFAULT;
        }
        break;
    case ECS_IOCTL_GET_ACCEL:
        if (copy_to_user(argp, &accel, sizeof(accel))) {
            AKMDBG("copy_to_user failed.");
            return -EFAULT;
        }
        break;
    default:
        break;
    }
    // printk(KERN_INFO"%s: ---\r\n", __func__);

    return 0;
}

static void akm8975_work_func(struct work_struct *work)
{
    char buffer[SENSOR_DATA_SIZE];
    int ret;
    // printk(KERN_INFO"%s\r\n", __func__);
    memset(buffer, 0, SENSOR_DATA_SIZE);
    buffer[0] = AK8975_REG_ST1;
    ret = AKI2C_RxData(buffer, SENSOR_DATA_SIZE);
    if (ret < 0) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s I2C failed\r\n", __func__);
        goto WORK_FUNC_END;
    }
    /* Check ST bit */
    if ((buffer[0] & 0x01) != 0x01) 
    {
        printk(KERN_ERR "akm8975_work_func: ST is not set\n");
        goto WORK_FUNC_END;
    }

    mutex_lock(&sense_data_mutex);
    memcpy(sense_data, buffer, SENSOR_DATA_SIZE);
    atomic_set(&data_ready, 1);
    wake_up(&data_ready_wq);
    mutex_unlock(&sense_data_mutex);

WORK_FUNC_END:
    enable_irq(this_client->irq);
    AKMFUNC("akm8975_work_func");
}

static irqreturn_t akm8975_interrupt(int irq, void *dev_id)
{
    struct akm8975_data *data = dev_id;
    disable_irq_nosync(this_client->irq);
    schedule_work(&data->work);
    return IRQ_HANDLED;
}

#ifdef CONFIG_PM
static int akm8975_suspend(struct device *dev)
{
#if AKM8975_PWROFF_EN
    struct akm8975_data *data = i2c_get_clientdata(this_client);
#endif

    printk(KERN_INFO"BJ_BSP_DRIVER: CP_COMPASS: %s\r\n", __func__);
    atomic_set(&suspend_flag, 1);
    atomic_set(&reserve_open_flag, atomic_read(&open_flag));
    atomic_set(&open_flag, 0);
    wake_up(&open_wq);
    disable_irq(this_client->irq);
    //gpio_free(70);

#if AKM8975_PWROFF_EN
    if (akm8975_power_on(data, false)) {
        pr_info(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s close power fail \r\n", __func__);
    }
#endif

    AKMDBG("suspended with flag=%d", atomic_read(&reserve_open_flag));

    return 0;
}

static int akm8975_resume(struct device *dev)
{
#if AKM8975_PWROFF_EN
    struct akm8975_data *data = i2c_get_clientdata(this_client);
#endif
    printk(KERN_INFO"BJ_BSP_DRIVER: CP_COMPASS: %s\r\n", __func__);

#if AKM8975_PWROFF_EN
    if(akm8975_power_on(data, true))
    {
        pr_info(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s open power fail \r\n", __func__);
    }
#endif

    //gpio_request(70, "akm8975_irq");
    enable_irq(this_client->irq);
    atomic_set(&suspend_flag, 0);
    atomic_set(&open_flag, atomic_read(&reserve_open_flag));
    wake_up(&open_wq);
    AKMDBG("resumed with flag=%d", atomic_read(&reserve_open_flag));

    return 0;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void akm8975_early_suspend(struct early_suspend *handler)
{
    printk(KERN_INFO"BJ_BSP_DRIVER: CP_COMPASS: %s\r\n", __func__);
    akm8975_suspend(this_client, PMSG_SUSPEND);
}

static void akm8975_early_resume(struct early_suspend *handler)
{
    printk(KERN_INFO"BJ_BSP_DRIVER: CP_COMPASS: %s\r\n", __func__);
    akm8975_resume(this_client);
}
#endif

static struct file_operations akmd_fops = {
    .owner = THIS_MODULE,
    .open = akmd_open,
    .release = akmd_release,
    .unlocked_ioctl = akmd_ioctl,
};

static struct file_operations akm_aot_fops = {
    .owner = THIS_MODULE,
    .open = akm_aot_open,
    .release = akm_aot_release,
    .unlocked_ioctl = akm_aot_ioctl,
};

static struct miscdevice akmd_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "akm8975_dev",
    .fops = &akmd_fops,
};

static struct miscdevice akm_aot_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "akm8975_aot",
    .fops = &akm_aot_fops,
};

int akm8975_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct akm8975_data *akm;
    int err = 0;

#ifdef CONFIG_LPM_MODE
    extern unsigned int poweroff_charging;
    extern unsigned int recovery_mode;
    if (1 == poweroff_charging || 1 == recovery_mode) {
        printk(KERN_ERR"%s: probe exit, lpm=%d recovery=%d\n", __func__, poweroff_charging, recovery_mode);
        return -ENODEV;
    }
#endif

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: check_functionality failed.\n");
        err = -ENODEV;
        goto exit0;
    }

    /* Allocate memory for driver data */
    akm = kzalloc(sizeof(struct akm8975_data), GFP_KERNEL);
    if (!akm) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: memory allocation failed.\n");
        err = -ENOMEM;
        goto exit1;
    }

    INIT_WORK(&akm->work, akm8975_work_func);
    i2c_set_clientdata(client, akm);
    akm->client = client;
    this_client = client;

    /* power on, modify by Jay.HF 2012-12-18 */
    err = akm8975_power_init(akm, true);
    if (err) {
        dev_err(&client->dev, "power init failed");
        goto exit1;
    }    

    err = akm8975_power_on(akm, true);
    if (err) {
        dev_err(&client->dev, "power on failed");
        goto exit1;
    }
    msleep(5);
    /* end */

    /* Check platform data*/
    if (client->dev.platform_data == NULL) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: platform data is NULL\n");
        err = -ENOMEM;
        goto exit2;
    }

    /* Copy to global variable */
    pdata = client->dev.platform_data;
    gpio_request(pdata->gpio_DRDY, "akm8975_irq");

    /* Check connection */
    err = AKECS_CheckDevice();

    if (err < 0) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: set power down mode error\n");
        goto exit3;
    }

    /* IRQ */
    err = request_irq(client->irq, akm8975_interrupt, IRQ_TYPE_EDGE_RISING,
                      "akm8975_DRDY", akm);
    if (err < 0) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: request irq failed\n");
        goto exit4;
    }

    /* Declare input device */
    akm->input_dev = input_allocate_device();
    if (!akm->input_dev) {
        err = -ENOMEM;
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: "
               "Failed to allocate input device\n");
        goto exit5;
    }
    /* Setup input device */
    set_bit(EV_ABS, akm->input_dev->evbit);
    /* yaw (0, 360) */
    input_set_abs_params(akm->input_dev, ABS_RX, 0, 23040, 0, 0);
    /* pitch (-180, 180) */
    input_set_abs_params(akm->input_dev, ABS_RY, -11520, 11520, 0, 0);
    /* roll (-90, 90) */
    input_set_abs_params(akm->input_dev, ABS_RZ, -5760, 5760, 0, 0);
    /* x-axis acceleration (720 x 8G) */
    input_set_abs_params(akm->input_dev, ABS_X, -5760, 5760, 0, 0);
    /* y-axis acceleration (720 x 8G) */
    input_set_abs_params(akm->input_dev, ABS_Y, -5760, 5760, 0, 0);
    /* z-axis acceleration (720 x 8G) */
    input_set_abs_params(akm->input_dev, ABS_Z, -5760, 5760, 0, 0);
#if 0
    /* temparature */
    input_set_abs_params(akm->input_dev, ABS_THROTTLE, -30, 85, 0, 0);
#endif
    /* status of magnetic sensor */
    input_set_abs_params(akm->input_dev, ABS_RUDDER, -32768, 3, 0, 0);
    /* status of acceleration sensor */
    input_set_abs_params(akm->input_dev, ABS_WHEEL, -32768, 3, 0, 0);
    /* x-axis of raw magnetic vector (-4096, 4095) */
    input_set_abs_params(akm->input_dev, ABS_HAT0X, -20480, 20479, 0, 0);
    /* y-axis of raw magnetic vector (-4096, 4095) */
    input_set_abs_params(akm->input_dev, ABS_HAT0Y, -20480, 20479, 0, 0);
    /* z-axis of raw magnetic vector (-4096, 4095) */
    input_set_abs_params(akm->input_dev, ABS_BRAKE, -20480, 20479, 0, 0);
    /* Set name */
    akm->input_dev->name = "compass";

    /* Register */
    err = input_register_device(akm->input_dev);
    if (err) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: "
               "Unable to register input device\n");
        goto exit6;
    }

    err = misc_register(&akmd_device);
    if (err) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: "
               "akmd_device register failed\n");
        goto exit7;
    }

    err = misc_register(&akm_aot_device);
    if (err) {
        printk(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: AKM8975 akm8975_probe: "
               "akm_aot_device register failed\n");
        goto exit8;
    }

    mutex_init(&sense_data_mutex);

    init_waitqueue_head(&data_ready_wq);
    init_waitqueue_head(&open_wq);

    /* As default, report no information */
    atomic_set(&m_flag, 0);
    atomic_set(&a_flag, 0);
    atomic_set(&mv_flag, 0);

#ifdef CONFIG_HAS_EARLYSUSPEND
    akm->akm_early_suspend.suspend = akm8975_early_suspend;
    akm->akm_early_suspend.resume = akm8975_early_resume;
    register_early_suspend(&akm->akm_early_suspend);
#endif

    akmd_accel[AKM_ACCEL_X] = 0.0f;
    akmd_accel[AKM_ACCEL_Y] = 0.0f;
    akmd_accel[AKM_ACCEL_Z] = 0.0f;

#if AKM8975_PWROFF_EN
    if (akm8975_power_on(akm, false)) {
        pr_info(KERN_ERR "BJ_BSP_DRIVER: CP_COMPASS: in %s akm8975_power_on fail \r\n", __func__);
    }
#endif
 
    return 0;

exit8:
    misc_deregister(&akmd_device);
exit7:
    input_unregister_device(akm->input_dev);
exit6:
    input_free_device(akm->input_dev);
exit5:
    free_irq(client->irq, akm);
exit4:
exit3:
exit2:
    akm8975_power_on(akm, false);
    akm8975_power_init(akm, false);
    kfree(akm);
exit1:
exit0:
    return err;
}

static int akm8975_remove(struct i2c_client *client)
{
    struct akm8975_data *akm = i2c_get_clientdata(client);
    AKMFUNC("akm8975_remove");

#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&akm->akm_early_suspend);
#endif

    misc_deregister(&akm_aot_device);
    misc_deregister(&akmd_device);
    input_unregister_device(akm->input_dev);
    free_irq(client->irq, akm);
    akm8975_power_on(akm, false);
    akm8975_power_init(akm, false);
    kfree(akm);
    AKMDBG("successfully removed.");
    return 0;
}

#if (defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND))
static const struct dev_pm_ops akm8975_pm_ops = {
    .suspend = akm8975_suspend,
    .resume  = akm8975_resume,
};
#else
static const struct dev_pm_ops akm8975_pm_ops = {
};
#endif

static const struct i2c_device_id akm8975_id[] = {
    {AKM8975_I2C_NAME, 0 },
    { }
};

static struct i2c_driver akm8975_driver = {
    .probe     = akm8975_probe,
    .remove    = akm8975_remove,
    .id_table  = akm8975_id,
    .driver    = {
        .name  = AKM8975_I2C_NAME,
        .owner = THIS_MODULE,
    #ifdef CONFIG_PM
        .pm    = &akm8975_pm_ops,
    #endif
    },
};


static int __init akm8975_init(void)
{
    int ret;
    pr_info(KERN_INFO"%s:\r\n", __func__);

    ret = i2c_add_driver(&akm8975_driver);
    return ret;
}

static void __exit akm8975_exit(void)
{
    i2c_del_driver(&akm8975_driver);
}

module_init(akm8975_init);
module_exit(akm8975_exit);

MODULE_AUTHOR("ZhangZhe@CoolPad");
MODULE_DESCRIPTION("AKM8975 Compass Driver");
MODULE_LICENSE("GPL");

