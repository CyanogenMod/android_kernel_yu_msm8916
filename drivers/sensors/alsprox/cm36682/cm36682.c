/* drivers/input/misc/CM36682.c - CM36682 optical sensors driver
 *
 * Copyright (C) 2012 Capella Microsystems Inc.
 *                                    
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/sensors/lightsensor.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/mach-types.h>
#include <linux/sensors/cm36682.h>
#include <linux/sensors/capella_cm3602.h>
#include <linux/sensors/alsprox_common.h>
#include <linux/sensors/sensparams.h>
#include <asm/setup.h>
#include <linux/wakelock.h>
#include <linux/jiffies.h>

#ifdef CONFIG_OF
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#endif

#include <linux/cpumask.h>
#include <linux/smp.h>

#define D(x...) pr_info(x)

#define I2C_RETRY_COUNT 2

#define NEAR_DELAY_TIME ((100 * HZ) / 1000)

#define CONTROL_INT_ISR_REPORT        0x00
#define CONTROL_ALS                   0x01
#define CONTROL_PS                    0x02

#define PS_CLOSE_VAL                  0x03
#define PS_AWAY_VAL                   0x05
#define CHANGE_SENSITIVITY            5 // in percent

static int record_init_fail = 0;
static void sensor_irq_do_work(struct work_struct *work);
static DECLARE_WORK(sensor_irq_work, sensor_irq_do_work);

struct CM36682_info {
	struct class *CM36682_class;
	struct device *ls_dev;
	struct device *ps_dev;

	struct input_dev *ls_input_dev;
	struct input_dev *ps_input_dev;

//#ifdef CONFIG_HAS_EARLYSUSPEND
#if 0 
	struct early_suspend early_suspend;
#endif

	struct i2c_client *i2c_client;
	struct workqueue_struct *lp_wq;

	int intr_pin;
	int als_enable;
	int ps_enable;
	int ps_irq_flag;

	uint16_t *adc_table;
	uint16_t cali_table[10];
	int irq;

	int ls_calibrate;
	
	int (*power)(int, uint8_t); /* power to the chip */

	uint32_t als_kadc;
	uint32_t als_gadc;
	uint16_t golden_adc;
	uint16_t cal_data;

	struct wake_lock ps_wake_lock;
	int psensor_opened;
	int lightsensor_opened;
	uint8_t slave_addr;

	uint8_t ps_close_thd_set;
	uint8_t ps_away_thd_set;	
	int current_level;
	uint16_t current_adc;

	uint16_t ps_conf1_val;
	uint16_t ps_conf3_val;

	uint16_t ls_cmd;
	uint8_t record_clear_int_fail;

	struct work_struct probe_work;
	struct CM36682_platform_data *pdata;
};

static struct CM36682_info *lp_info;
static int fLevel=-1;
static struct mutex als_enable_mutex, als_disable_mutex, als_get_adc_mutex;
static struct mutex ps_enable_mutex, ps_disable_mutex, ps_get_adc_mutex;
static struct mutex CM36682_control_mutex;
static int lightsensor_enable(struct CM36682_info *lpi);
static int lightsensor_disable(struct CM36682_info *lpi);
static int initial_CM36682(struct CM36682_info *lpi);
static void psensor_initial_cmd(struct CM36682_info *lpi);

static int32_t als_kadc;

static int control_and_report(struct CM36682_info *lpi, uint8_t mode, uint16_t param);

#ifdef CONFIG_OF
static int CM36682_parse_dt(struct device *dev, struct CM36682_platform_data *pdata)
{
	int i, ret;
	u32 value;
	struct device_node *np = dev->of_node;
	struct property *prop;
	enum of_gpio_flags flags;
	u32 levels[10];

	pdata->power = NULL;
	prop = of_find_property(np, "capella,levels", NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	ret = of_property_read_u32_array(np, "capella,levels", levels, prop->length/sizeof(u32));
	if (ret && (ret != -EINVAL)) {
		dev_err(dev, "%s: Unable to read %s\n", __func__, "capella,levels");
		return ret;
	}
	for(i = 0; i < 10; i++) {
		pdata->levels[i] = levels[i];
	}

	value = of_get_named_gpio_flags(np, "capella,intr", 0, &flags);
	pdata->intr = value;
	printk("capella,intr = %d\n", (int)pdata->intr);
	if (pdata->intr < 0) {
		return -EINVAL;
	}
	gpio_request(pdata->intr, "gpio_CM36682_intr");
	gpio_direction_input(pdata->intr);

	ret = of_property_read_u32(np, "capella,slave_addr", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,slave_addr", np->full_name);
		return -ENODEV;
	}
	pdata->slave_addr = value;
	printk("capella,slave_addr = %d\n", (int)pdata->slave_addr);
	
	ret = of_property_read_u32(np, "capella,ps_close_thd_set", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,ps_close_thd_set", np->full_name);
		return -ENODEV;
	}
	pdata->ps_close_thd_set = value;
	printk("capella,ps_close_thd_set = %d\n", pdata->ps_close_thd_set);
	
	ret = of_property_read_u32(np, "capella,ps_away_thd_set", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,ps_away_thd_set", np->full_name);
		return -ENODEV;
	}
	pdata->ps_away_thd_set = value;
	printk("capella,ps_away_thd_set = %d\n", pdata->ps_away_thd_set);
	
	ret = of_property_read_u32(np, "capella,ls_cmd", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,ls_cmd", np->full_name);
		return -ENODEV;
	}
	pdata->ls_cmd = value;
	printk("capella,ls_cmd = %d\n", pdata->ls_cmd);
	
	ret = of_property_read_u32(np, "capella,ps_conf1_val", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,ps_conf1_val", np->full_name);
		return -ENODEV;
	}
	pdata->ps_conf1_val = value;
	printk("capella,ps_conf1_val = %d\n", pdata->ps_conf1_val);

	ret = of_property_read_u32(np, "capella,ps_conf3_val", &value);
	if (ret) {
		dev_err(dev, "Looking up %s property in node %s failed",
			"capella,ps_conf3_val", np->full_name);
		return -ENODEV;
	}
	pdata->ps_conf3_val = value;
	printk("capella,ps_conf3_val = %d\n", pdata->ps_conf3_val);
#if 0
	ret = of_property_read_string(np, "atmel,vcc_i2c_supply", &pdata->ts_vcc_i2c);
	ret = of_property_read_string(np, "atmel,vdd_ana_supply", &pdata->ts_vdd_ana);
	printk("pwr_en=%d, sleep_pwr_en=%d, vcc_i2c=%s, vdd_ana=%s\n", pdata->pwr_en,
				pdata->sleep_pwr_en, pdata->ts_vcc_i2c, pdata->ts_vdd_ana);
#endif
	return 0;
}
#endif

static int I2C_RxData(uint16_t slaveAddr, uint8_t cmd, uint8_t *rxData, int length)
{
	uint8_t loop_i;
	int val;
	struct CM36682_info *lpi = lp_info;
	uint8_t subaddr[1];
	struct i2c_msg msgs[] = {
		{
		 .addr = slaveAddr,
		 .flags = 0,
		 .len = 1,
		 .buf = subaddr,
		 },
		{
		 .addr = slaveAddr,
		 .flags = I2C_M_RD,
		 .len = length,
		 .buf = rxData,
		 },		 
	};
	subaddr[0] = cmd;

	for (loop_i = 0; loop_i < I2C_RETRY_COUNT; loop_i++) {

		if (i2c_transfer(lp_info->i2c_client->adapter, msgs, 2) > 0)
			break;

		val = gpio_get_value(lpi->intr_pin);
		/*check intr GPIO when i2c error*/
		if (loop_i == 0 || loop_i == I2C_RETRY_COUNT -1)
			D("[PS][CM36682 error] %s, i2c err, slaveAddr 0x%x ISR gpio %d  = %d, record_init_fail %d \n",
				__func__, slaveAddr, lpi->intr_pin, val, record_init_fail);

		msleep(10);
	}
	if (loop_i >= I2C_RETRY_COUNT) {
		printk(KERN_ERR "[PS_ERR][CM36682 error] %s retry over %d\n",
			__func__, I2C_RETRY_COUNT);
		return -EIO;
	}

	return 0;
}

static int I2C_TxData(uint16_t slaveAddr, uint8_t *txData, int length)
{
	uint8_t loop_i;
	int val;
	struct CM36682_info *lpi = lp_info;
	struct i2c_msg msg[] = {
		{
		 .addr = slaveAddr,
		 .flags = 0,
		 .len = length,
		 .buf = txData,
		 },
	};

	for (loop_i = 0; loop_i < I2C_RETRY_COUNT; loop_i++) {
		if (i2c_transfer(lp_info->i2c_client->adapter, msg, 1) > 0)
			break;

		val = gpio_get_value(lpi->intr_pin);
		/*check intr GPIO when i2c error*/
		if (loop_i == 0 || loop_i == I2C_RETRY_COUNT -1)
			D("[PS][CM36682 error] %s, i2c err, slaveAddr 0x%x, value 0x%x, ISR gpio%d  = %d, record_init_fail %d\n",
				__func__, slaveAddr, txData[0], lpi->intr_pin, val, record_init_fail);

		msleep(10);
	}

	if (loop_i >= I2C_RETRY_COUNT) {
		printk(KERN_ERR "[PS_ERR][CM36682 error] %s retry over %d\n",
			__func__, I2C_RETRY_COUNT);
		return -EIO;
	}

	return 0;
}

static int _CM36682_I2C_Read_Word(uint16_t slaveAddr, uint8_t cmd, uint16_t *pdata)
{
	uint8_t buffer[2];
	int ret = 0;

	if (pdata == NULL)
		return -EFAULT;

	ret = I2C_RxData(slaveAddr, cmd, buffer, 2);
	if (ret < 0) {
		pr_err(
			"[PS_ERR][CM36682 error]%s: I2C_RxData fail [0x%x, 0x%x]\n",
			__func__, slaveAddr, cmd);
		return ret;
	}

	*pdata = (buffer[1]<<8)|buffer[0];
#if 0
	/* Debug use */
	printk(KERN_DEBUG "[CM36682] %s: I2C_RxData[0x%x, 0x%x] = 0x%x\n",
		__func__, slaveAddr, cmd, *pdata);
#endif
	return ret;
}

static int _CM36682_I2C_Write_Word(uint16_t SlaveAddress, uint8_t cmd, uint16_t data)
{
	char buffer[3];
	int ret = 0;
#if 0
	/* Debug use */
	printk(KERN_DEBUG
	"[CM36682] %s: _CM36682_I2C_Write_Word[0x%x, 0x%x, 0x%x]\n",
		__func__, SlaveAddress, cmd, data);
#endif
	buffer[0] = cmd;
	buffer[1] = (uint8_t)(data&0xff);
	buffer[2] = (uint8_t)((data&0xff00)>>8);	
	
	ret = I2C_TxData(SlaveAddress, buffer, 3);
	if (ret < 0) {
		pr_err("[PS_ERR][CM36682 error]%s: I2C_TxData fail\n", __func__);
		return -EIO;
	}

	return ret;
}

static int get_ls_adc_value(uint16_t *als_step, bool resume)
{
	struct CM36682_info *lpi = lp_info;
	uint32_t tmpResult;
	int ret = 0;

	if (als_step == NULL)
		return -EFAULT;

	/* Read ALS data: */
	ret = _CM36682_I2C_Read_Word(lpi->slave_addr, ALS_DATA, als_step);
	if (ret < 0) {
		pr_err(
			"[LS][CM36682 error]%s: _CM36682_I2C_Read_Word fail\n",
			__func__);
		return -EIO;
	}

  if (!lpi->ls_calibrate) {
		tmpResult = (uint32_t)(*als_step) * lpi->als_gadc / lpi->als_kadc;
		if (tmpResult > 0xFFFF)
			*als_step = 0xFFFF;
		else
		  *als_step = tmpResult;  			
	}

	D("[LS][CM36682] %s: raw adc = 0x%X, ls_calibrate = %d\n",
		__func__, *als_step, lpi->ls_calibrate);

	return ret;
}

static int set_lsensor_range(uint16_t low_thd, uint16_t high_thd)
{
	int ret = 0;
	struct CM36682_info *lpi = lp_info;

	_CM36682_I2C_Write_Word(lpi->slave_addr, ALS_THDH, high_thd);
	_CM36682_I2C_Write_Word(lpi->slave_addr, ALS_THDL, low_thd);

	return ret;
}

static int get_ps_adc_value(uint16_t *data)
{
	int ret = 0;
	struct CM36682_info *lpi = lp_info;

	if (data == NULL)
		return -EFAULT;	

	ret = _CM36682_I2C_Read_Word(lpi->slave_addr, PS_DATA, data);
	
	(*data) &= 0xFF;
	
	if (ret < 0) {
		pr_err(
			"[PS][CM36682 error]%s: _CM36682_I2C_Read_Word fail\n",
			__func__);
		return -EIO;
	} else {
		pr_err(
			"[PS][CM36682 OK]%s: _CM36682_I2C_Read_Word OK 0x%x\n",
			__func__, *data);
	}

	return ret;
}

static uint16_t mid_value(uint16_t value[], uint8_t size)
{
	int i = 0, j = 0;
	uint16_t temp = 0;

	if (size < 3)
		return 0;

	for (i = 0; i < (size - 1); i++)
		for (j = (i + 1); j < size; j++)
			if (value[i] > value[j]) {
				temp = value[i];
				value[i] = value[j];
				value[j] = temp;
			}
	return value[((size - 1) / 2)];
}

static int get_stable_ps_adc_value(uint16_t *ps_adc)
{
	uint16_t value[3] = {0, 0, 0}, mid_val = 0;
	int ret = 0;
	int i = 0;
	int wait_count = 0;
	struct CM36682_info *lpi = lp_info;

	for (i = 0; i < 3; i++) {
		/*wait interrupt GPIO high*/
		while (gpio_get_value(lpi->intr_pin) == 0) {
			msleep(10);
			wait_count++;
			if (wait_count > 12) {
				pr_err("[PS_ERR][CM36682 error]%s: interrupt GPIO low,"
					" get_ps_adc_value\n", __func__);
				return -EIO;
			}
		}

		ret = get_ps_adc_value(&value[i]);
		if (ret < 0) {
			pr_err("[PS_ERR][CM36682 error]%s: get_ps_adc_value\n",
				__func__);
			return -EIO;
		}

		if (wait_count < 60/10) {/*wait gpio less than 60ms*/
			msleep(60 - (10*wait_count));
		}
		wait_count = 0;
	}

	/*D("Sta_ps: Before sort, value[0, 1, 2] = [0x%x, 0x%x, 0x%x]",
		value[0], value[1], value[2]);*/
	mid_val = mid_value(value, 3);
	D("Sta_ps: After sort, value[0, 1, 2] = [0x%x, 0x%x, 0x%x]",
		value[0], value[1], value[2]);
	*ps_adc = (mid_val & 0xFF);

	return 0;
}

static void sensor_irq_do_work(struct work_struct *work)
{
	struct CM36682_info *lpi = lp_info;
	uint16_t intFlag;
  _CM36682_I2C_Read_Word(lpi->slave_addr, INT_FLAG, &intFlag);
	control_and_report(lpi, CONTROL_INT_ISR_REPORT, intFlag);  
	  
	enable_irq(lpi->irq);
}

static irqreturn_t CM36682_irq_handler(int irq, void *data)
{
	struct CM36682_info *lpi = data;

	disable_irq_nosync(lpi->irq);
	queue_work(lpi->lp_wq, &sensor_irq_work);
	return IRQ_HANDLED;
}

static int als_power(int enable)
{
	struct CM36682_info *lpi = lp_info;

	if (lpi->power)
		lpi->power(LS_PWR_ON, 1);

	return 0;
}

static void ls_initial_cmd(struct CM36682_info *lpi)
{	
	/*must disable l-sensor interrupt befrore IST create*//*disable ALS func*/
	lpi->ls_cmd &= CM36682_ALS_INT_MASK;
  lpi->ls_cmd |= CM36682_ALS_SD;
  _CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);  
}

static void psensor_initial_cmd(struct CM36682_info *lpi)
{
	/*must disable p-sensor interrupt befrore IST create*//*disable ALS func*/		
  lpi->ps_conf1_val |= CM36682_PS_SD;
  lpi->ps_conf1_val &= CM36682_PS_INT_MASK;  
  _CM36682_I2C_Write_Word(lpi->slave_addr, PS_CONF1, lpi->ps_conf1_val);   
  _CM36682_I2C_Write_Word(lpi->slave_addr, PS_CONF3, lpi->ps_conf3_val);
  _CM36682_I2C_Write_Word(lpi->slave_addr, PS_THD, (lpi->ps_close_thd_set <<8)| lpi->ps_away_thd_set);

	D("[PS][CM36682] %s, finish\n", __func__);	
}

static int psensor_enable(struct CM36682_info *lpi)
{
	int ret = -EIO;
	
	mutex_lock(&ps_enable_mutex);
	D("[PS][CM36682] %s\n", __func__);

	if ( lpi->ps_enable ) {
		D("[PS][CM36682] %s: already enabled\n", __func__);
		ret = 0;
	} else
  	ret = control_and_report(lpi, CONTROL_PS, 1);
	
	mutex_unlock(&ps_enable_mutex);
	return ret;
}

static int psensor_disable(struct CM36682_info *lpi)
{
	int ret = -EIO;
	
	mutex_lock(&ps_disable_mutex);
	D("[PS][CM36682] %s\n", __func__);

	if ( lpi->ps_enable == 0 ) {
		D("[PS][CM36682] %s: already disabled\n", __func__);
		ret = 0;
	} else
  	ret = control_and_report(lpi, CONTROL_PS,0);
	
	mutex_unlock(&ps_disable_mutex);
	return ret;
}

static int psensor_cal_prox_threshold(struct CM36682_info *lpi, int adc_value)
{
	int ret = -EIO;

	D("[PS][CM36682] %s\n", __func__);
	if (adc_value < 10) {
		lpi->ps_close_thd_set = adc_value + 3;
		lpi->ps_away_thd_set = adc_value + 1;
	}
	else {
		lpi->ps_close_thd_set = adc_value + 8;
		lpi->ps_away_thd_set = adc_value + 4;
	}

	return ret;
}

static int psensor_open(struct inode *inode, struct file *file)
{
	struct CM36682_info *lpi = lp_info;

	D("[PS][CM36682] %s\n", __func__);

	if (lpi->psensor_opened)
		return -EBUSY;

	lpi->psensor_opened = 1;

	return 0;
}

static int psensor_release(struct inode *inode, struct file *file)
{
	struct CM36682_info *lpi = lp_info;

	D("[PS][CM36682] %s\n", __func__);

	lpi->psensor_opened = 0;

	return psensor_disable(lpi);
	//return 0;
}

static long psensor_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	int val;
	uint16_t prox_avg;
	uint8_t prox_param[4];
	struct prox_offset cal_data;
	struct CM36682_info *lpi = lp_info;
	extern int sensparams_write_to_flash(int type, unsigned char *in, int len);
	D("[PS][CM36682] %s cmd %d\n", __func__, _IOC_NR(cmd));

	switch (cmd) {
	case CAPELLA_CM3602_IOCTL_ENABLE:
		if (get_user(val, (unsigned long __user *)arg))
			return -EFAULT;
		if (val)
			return psensor_enable(lpi);
		else
			return psensor_disable(lpi);
		break;

	case CAPELLA_CM3602_IOCTL_GET_ENABLED:
		return put_user(lpi->ps_enable, (unsigned long __user *)arg);
		break;

	case CAPELLA_CM3602_IOCTL_CALIBRATE:
		if ((struct prox_offset *)arg != NULL) {
			get_stable_ps_adc_value(&prox_avg);
			psensor_cal_prox_threshold(lpi, prox_avg);
			memset(prox_param, 0, sizeof(prox_param));
			prox_param[0] = (prox_avg < 200) ? 0x01:0x02;
			prox_param[1] = lpi->ps_close_thd_set;
			prox_param[2] = lpi->ps_away_thd_set;
			prox_param[3] = prox_avg;
			//sensparams_write_to_flash(SENSPARAMS_TYPE_PROX, prox_param, sizeof(prox_param));
			cal_data.x = lpi->ps_close_thd_set;
			cal_data.y = lpi->ps_away_thd_set;
			cal_data.z = prox_avg;
			if(copy_to_user((struct prox_offset *)arg, &cal_data, sizeof(cal_data))) {
				D("[PS][CM36682] %s data trans error,use default offset !\n", __func__);
			}
			D("[PS][CM36682] %s prox_hig = [%d], prox_low = [%d], prox_avg = [%d]\n", __func__,
					lpi->ps_close_thd_set, lpi->ps_away_thd_set, prox_avg);
		}else {
			pr_err("[PS][CM36682 error]%s: (%d) null pointer !\n", __func__, __LINE__);
			return -EINVAL;
		}
		break;

	default:
		pr_err("[PS][CM36682 error]%s: invalid cmd %d\n", __func__, _IOC_NR(cmd));
		return -EINVAL;
	}
	return 0;
}

static const struct file_operations psensor_fops = {
	.owner = THIS_MODULE,
	.open = psensor_open,
	.release = psensor_release,
	.unlocked_ioctl = psensor_ioctl
};

struct miscdevice psensor_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = CM36682_PS_NAME, //"cm36682-ps",
	.fops = &psensor_fops
};

static void lightsensor_set_kvalue(struct CM36682_info *lpi)
{
	if (!lpi) {
		pr_err("[LS][CM36682 error]%s: ls_info is empty\n", __func__);
		return;
	}

	D("[LS][CM36682] %s: ALS calibrated als_kadc=0x%x\n",
			__func__, als_kadc);

	if (als_kadc >> 16 == ALS_CALIBRATED) {
		lpi->als_kadc = als_kadc & 0xFFFF;
	}
	else {
		lpi->als_kadc = 0;
		D("[LS][CM36682] %s: no ALS calibrated\n", __func__);
	}

	if (lpi->als_kadc && lpi->golden_adc > 0) {
		lpi->als_kadc = (lpi->als_kadc > 0 && lpi->als_kadc < 0x1000) ?
				lpi->als_kadc : lpi->golden_adc;
		lpi->als_gadc = lpi->golden_adc;
	} else {
		lpi->als_kadc = 1;
		lpi->als_gadc = 1;
	}
	D("[LS][CM36682] %s: als_kadc=0x%x, als_gadc=0x%x\n",
		__func__, lpi->als_kadc, lpi->als_gadc);
}


static int lightsensor_update_table(struct CM36682_info *lpi)
{
	uint32_t tmpData[10];
	int i;
	for (i = 0; i < 10; i++) {
		tmpData[i] = (uint32_t)(*(lpi->adc_table + i))
				* lpi->als_kadc / lpi->als_gadc ;
		if( tmpData[i] <= 0xFFFF ){
      lpi->cali_table[i] = (uint16_t) tmpData[i];		
    } else {
      lpi->cali_table[i] = 0xFFFF;    
    }         
		D("[LS][CM36682] %s: Calibrated adc_table: data[%d], %x\n",
			__func__, i, lpi->cali_table[i]);
	}

	return 0;
}


static int lightsensor_enable(struct CM36682_info *lpi)
{
	int ret = -EIO;
	
	mutex_lock(&als_enable_mutex);
	D("[LS][CM36682] %s\n", __func__);

	if (lpi->als_enable) {
		D("[LS][CM36682] %s: already enabled\n", __func__);
		ret = 0;
	} else
  	ret = control_and_report(lpi, CONTROL_ALS, 1);
	
	mutex_unlock(&als_enable_mutex);
	return ret;
}

static int lightsensor_disable(struct CM36682_info *lpi)
{
	int ret = -EIO;
	mutex_lock(&als_disable_mutex);
	D("[LS][CM36682] %s\n", __func__);

	if ( lpi->als_enable == 0 ) {
		D("[LS][CM36682] %s: already disabled\n", __func__);
		ret = 0;
	} else
    ret = control_and_report(lpi, CONTROL_ALS, 0);
	
	mutex_unlock(&als_disable_mutex);
	return ret;
}

static int lightsensor_open(struct inode *inode, struct file *file)
{
	struct CM36682_info *lpi = lp_info;
	int rc = 0;

	D("[LS][CM36682] %s\n", __func__);
	if (lpi->lightsensor_opened) {
		pr_err("[LS][CM36682 error]%s: already opened\n", __func__);
		rc = -EBUSY;
	}
	lpi->lightsensor_opened = 1;
	return rc;
}

static int lightsensor_release(struct inode *inode, struct file *file)
{
	struct CM36682_info *lpi = lp_info;

	D("[LS][CM36682] %s\n", __func__);
	lpi->lightsensor_opened = 0;
	return 0;
}

static long lightsensor_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int rc, val;
	struct CM36682_info *lpi = lp_info;

	/*D("[CM36682] %s cmd %d\n", __func__, _IOC_NR(cmd));*/

	switch (cmd) {
	case LIGHTSENSOR_IOCTL_ENABLE:
		if (get_user(val, (unsigned long __user *)arg)) {
			rc = -EFAULT;
			break;
		}
		D("[LS][CM36682] %s LIGHTSENSOR_IOCTL_ENABLE, value = %d\n",
			__func__, val);
		rc = val ? lightsensor_enable(lpi) : lightsensor_disable(lpi);
		break;
	case LIGHTSENSOR_IOCTL_GET_ENABLED:
		val = lpi->als_enable;
		D("[LS][CM36682] %s LIGHTSENSOR_IOCTL_GET_ENABLED, enabled %d\n",
			__func__, val);
		rc = put_user(val, (unsigned long __user *)arg);
		break;
	default:
		pr_err("[LS][CM36682 error]%s: invalid cmd %d\n",
			__func__, _IOC_NR(cmd));
		rc = -EINVAL;
	}

	return rc;
}

static const struct file_operations lightsensor_fops = {
	.owner = THIS_MODULE,
	.open = lightsensor_open,
	.release = lightsensor_release,
	.unlocked_ioctl = lightsensor_ioctl
};

static struct miscdevice lightsensor_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = CM36682_LS_NAME, //"cm36682-ls",
	.fops = &lightsensor_fops
};


static ssize_t ps_adc_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{

	uint16_t value;
	int ret;
	struct CM36682_info *lpi = lp_info;
	int intr_val;

	intr_val = gpio_get_value(lpi->intr_pin);

	get_ps_adc_value(&value);

	ret = sprintf(buf, "ADC[0x%04X], ENABLE = %d, intr_pin = %d\n", value, lpi->ps_enable, intr_val);

	return ret;
}

static ssize_t ps_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ps_en;
	struct CM36682_info *lpi = lp_info;

	ps_en = -1;
	sscanf(buf, "%d", &ps_en);

	if (ps_en != 0 && ps_en != 1
		&& ps_en != 10 && ps_en != 13 && ps_en != 16)
		return -EINVAL;

	if (ps_en) {
		D("[PS][CM36682] %s: ps_en=%d\n",
			__func__, ps_en);
		psensor_enable(lpi);
	} else
		psensor_disable(lpi);

	D("[PS][CM36682] %s\n", __func__);

	return count;
}

static DEVICE_ATTR(ps_adc, 0664, ps_adc_show, ps_enable_store);

unsigned PS_cmd_test_value;
static ssize_t ps_parameters_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	struct CM36682_info *lpi = lp_info;

	ret = sprintf(buf, "PS_close_thd_set = 0x%x, PS_away_thd_set = 0x%x, PS_cmd_cmd:value = 0x%x\n",
		lpi->ps_close_thd_set, lpi->ps_away_thd_set, PS_cmd_test_value);

	return ret;
}

static ssize_t ps_parameters_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{

	struct CM36682_info *lpi = lp_info;
	char *token[10];
	int i;

	printk(KERN_INFO "[PS][CM36682] %s\n", buf);
	for (i = 0; i < 3; i++)
		token[i] = strsep((char **)&buf, " ");

	lpi->ps_close_thd_set = simple_strtoul(token[0], NULL, 16);
	lpi->ps_away_thd_set = simple_strtoul(token[1], NULL, 16);	
	PS_cmd_test_value = simple_strtoul(token[2], NULL, 16);
	printk(KERN_INFO
		"[PS][CM36682]Set PS_close_thd_set = 0x%x, PS_away_thd_set = 0x%x, PS_cmd_cmd:value = 0x%x\n",
		lpi->ps_close_thd_set, lpi->ps_away_thd_set, PS_cmd_test_value);

	D("[PS][CM36682] %s\n", __func__);

	return count;
}

static DEVICE_ATTR(ps_parameters, 0664,
	ps_parameters_show, ps_parameters_store);


static ssize_t ps_conf_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct CM36682_info *lpi = lp_info;
	return sprintf(buf, "PS_CONF1 = 0x%x, PS_CONF3 = 0x%x\n", lpi->ps_conf1_val, lpi->ps_conf3_val);
}
static ssize_t ps_conf_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int code1, code2;
	struct CM36682_info *lpi = lp_info;

	sscanf(buf, "0x%x 0x%x", &code1, &code2);

	D("[PS]%s: store value PS conf1 reg = 0x%x PS conf3 reg = 0x%x\n", __func__, code1, code2);

  lpi->ps_conf1_val = code1;
  lpi->ps_conf3_val = code2;

	_CM36682_I2C_Write_Word(lpi->slave_addr, PS_CONF3, lpi->ps_conf3_val );  
	_CM36682_I2C_Write_Word(lpi->slave_addr, PS_CONF1, lpi->ps_conf1_val );

	return count;
}
static DEVICE_ATTR(ps_conf, 0664, ps_conf_show, ps_conf_store);

static ssize_t ps_thd_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	struct CM36682_info *lpi = lp_info;
  ret = sprintf(buf, "%s ps_close_thd_set = 0x%x, ps_away_thd_set = 0x%x\n", __func__, lpi->ps_close_thd_set, lpi->ps_away_thd_set);
  return ret;	
}
static ssize_t ps_thd_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int code;
	struct CM36682_info *lpi = lp_info;

	sscanf(buf, "0x%x", &code);

	D("[PS]%s: store value = 0x%x\n", __func__, code);

	lpi->ps_away_thd_set = code &0xFF;
	lpi->ps_close_thd_set = (code &0xFF00)>>8;	

	D("[PS]%s: ps_close_thd_set = 0x%x, ps_away_thd_set = 0x%x\n", __func__, lpi->ps_close_thd_set, lpi->ps_away_thd_set);

	return count;
}
static DEVICE_ATTR(ps_thd, 0664, ps_thd_show, ps_thd_store);

static ssize_t ps_hw_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret = 0;
	struct CM36682_info *lpi = lp_info;

	ret = sprintf(buf, "PS1: reg = 0x%x, PS3: reg = 0x%x, ps_close_thd_set = 0x%x, ps_away_thd_set = 0x%x\n",
		lpi->ps_conf1_val, lpi->ps_conf3_val, lpi->ps_close_thd_set, lpi->ps_away_thd_set);

	return ret;
}
static ssize_t ps_hw_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int code;
//	struct CM36682_info *lpi = lp_info;

	sscanf(buf, "0x%x", &code);

	D("[PS]%s: store value = 0x%x\n", __func__, code);

	return count;
}
static DEVICE_ATTR(ps_hw, 0664, ps_hw_show, ps_hw_store);

static ssize_t ls_adc_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int ret;
	struct CM36682_info *lpi = lp_info;

	D("[LS][CM36682] %s: ADC = 0x%04X, Level = %d \n",
		__func__, lpi->current_adc, lpi->current_level);
	ret = sprintf(buf, "ADC[0x%04X] => level %d\n",
		lpi->current_adc, lpi->current_level);

	return ret;
}

static DEVICE_ATTR(ls_adc, 0664, ls_adc_show, NULL);

static ssize_t ls_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{

	int ret = 0;
	struct CM36682_info *lpi = lp_info;

	ret = sprintf(buf, "Light sensor Auto Enable = %d\n",
			lpi->als_enable);

	return ret;
}

static ssize_t ls_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret = 0;
	int ls_auto;
	struct CM36682_info *lpi = lp_info;

	ls_auto = -1;
	sscanf(buf, "%d", &ls_auto);

	if (ls_auto != 0 && ls_auto != 1 && ls_auto != 147)
		return -EINVAL;

	if (ls_auto) {
		lpi->ls_calibrate = (ls_auto == 147) ? 1 : 0;
		ret = lightsensor_enable(lpi);
	} else {
		lpi->ls_calibrate = 0;
		ret = lightsensor_disable(lpi);
	}

	D("[LS][CM36682] %s: lpi->als_enable = %d, lpi->ls_calibrate = %d, ls_auto=%d\n",
		__func__, lpi->als_enable, lpi->ls_calibrate, ls_auto);

	if (ret < 0)
		pr_err(
		"[LS][CM36682 error]%s: set auto light sensor fail\n",
		__func__);

	return count;
}

static DEVICE_ATTR(ls_auto, 0664,
	ls_enable_show, ls_enable_store);

static ssize_t ls_kadc_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct CM36682_info *lpi = lp_info;
	int ret;

	ret = sprintf(buf, "kadc = 0x%x",
			lpi->als_kadc);

	return ret;
}

static ssize_t ls_kadc_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct CM36682_info *lpi = lp_info;
	int kadc_temp = 0;

	sscanf(buf, "%d", &kadc_temp);

	mutex_lock(&als_get_adc_mutex);
  if(kadc_temp != 0) {
		lpi->als_kadc = kadc_temp;
		if(  lpi->als_gadc != 0){
  		if (lightsensor_update_table(lpi) < 0)
				printk(KERN_ERR "[LS][CM36682 error] %s: update ls table fail\n", __func__);
  	} else {
			printk(KERN_INFO "[LS]%s: als_gadc =0x%x wait to be set\n",
					__func__, lpi->als_gadc);
  	}		
	} else {
		printk(KERN_INFO "[LS]%s: als_kadc can't be set to zero\n",
				__func__);
	}
				
	mutex_unlock(&als_get_adc_mutex);
	return count;
}

static DEVICE_ATTR(ls_kadc, 0664, ls_kadc_show, ls_kadc_store);

static ssize_t ls_gadc_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct CM36682_info *lpi = lp_info;
	int ret;

	ret = sprintf(buf, "gadc = 0x%x\n", lpi->als_gadc);

	return ret;
}

static ssize_t ls_gadc_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct CM36682_info *lpi = lp_info;
	int gadc_temp = 0;

	sscanf(buf, "%d", &gadc_temp);
	
	mutex_lock(&als_get_adc_mutex);
  if(gadc_temp != 0) {
		lpi->als_gadc = gadc_temp;
		if(  lpi->als_kadc != 0){
  		if (lightsensor_update_table(lpi) < 0)
				printk(KERN_ERR "[LS][CM36682 error] %s: update ls table fail\n", __func__);
  	} else {
			printk(KERN_INFO "[LS]%s: als_kadc =0x%x wait to be set\n",
					__func__, lpi->als_kadc);
  	}		
	} else {
		printk(KERN_INFO "[LS]%s: als_gadc can't be set to zero\n",
				__func__);
	}
	mutex_unlock(&als_get_adc_mutex);
	return count;
}

static DEVICE_ATTR(ls_gadc, 0664, ls_gadc_show, ls_gadc_store);

static ssize_t ls_adc_table_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	unsigned length = 0;
	int i;

	for (i = 0; i < 10; i++) {
		length += sprintf(buf + length,
			"[CM36682]Get adc_table[%d] =  0x%x ; %d, Get cali_table[%d] =  0x%x ; %d, \n",
			i, *(lp_info->adc_table + i),
			*(lp_info->adc_table + i),
			i, *(lp_info->cali_table + i),
			*(lp_info->cali_table + i));
	}
	return length;
}

static ssize_t ls_adc_table_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{

	struct CM36682_info *lpi = lp_info;
	char *token[10];
	uint16_t tempdata[10];
	int i;

	printk(KERN_INFO "[LS][CM36682]%s\n", buf);
	for (i = 0; i < 10; i++) {
		token[i] = strsep((char **)&buf, " ");
		tempdata[i] = simple_strtoul(token[i], NULL, 16);
		if (tempdata[i] < 1 || tempdata[i] > 0xffff) {
			printk(KERN_ERR
			"[LS][CM36682 error] adc_table[%d] =  0x%x Err\n",
			i, tempdata[i]);
			return count;
		}
	}
	mutex_lock(&als_get_adc_mutex);
	for (i = 0; i < 10; i++) {
		lpi->adc_table[i] = tempdata[i];
		printk(KERN_INFO
		"[LS][CM36682]Set lpi->adc_table[%d] =  0x%x\n",
		i, *(lp_info->adc_table + i));
	}
	if (lightsensor_update_table(lpi) < 0)
		printk(KERN_ERR "[LS][CM36682 error] %s: update ls table fail\n",
		__func__);
	mutex_unlock(&als_get_adc_mutex);
	D("[LS][CM36682] %s\n", __func__);

	return count;
}

static DEVICE_ATTR(ls_adc_table, 0664,
	ls_adc_table_show, ls_adc_table_store);

static ssize_t ls_conf_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct CM36682_info *lpi = lp_info;
	return sprintf(buf, "ALS_CONF = %x\n", lpi->ls_cmd);
}
static ssize_t ls_conf_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct CM36682_info *lpi = lp_info;
	int value = 0;
	sscanf(buf, "0x%x", &value);

	lpi->ls_cmd = value;
	printk(KERN_INFO "[LS]set ALS_CONF = %x\n", lpi->ls_cmd);
	
	_CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);
	return count;
}
static DEVICE_ATTR(ls_conf, 0664, ls_conf_show, ls_conf_store);

static ssize_t ls_fLevel_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "fLevel = %d\n", fLevel);
}
static ssize_t ls_fLevel_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct CM36682_info *lpi = lp_info;
	int value=0;
	sscanf(buf, "%d", &value);
	(value>=0)?(value=min(value,10)):(value=max(value,-1));
	fLevel=value;
	input_report_abs(lpi->ls_input_dev, ABS_MISC, fLevel);
	input_sync(lpi->ls_input_dev);
	printk(KERN_INFO "[LS]set fLevel = %d\n", fLevel);

	msleep(1000);
	fLevel=-1;
	return count;
}
static DEVICE_ATTR(ls_flevel, 0664, ls_fLevel_show, ls_fLevel_store);

static int lightsensor_setup(struct CM36682_info *lpi)
{
	int ret;

	lpi->ls_input_dev = input_allocate_device();
	if (!lpi->ls_input_dev) {
		pr_err(
			"[LS][CM36682 error]%s: could not allocate ls input device\n",
			__func__);
		return -ENOMEM;
	}
	lpi->ls_input_dev->name = "CM36682-ls";
	set_bit(EV_ABS, lpi->ls_input_dev->evbit);
	input_set_abs_params(lpi->ls_input_dev, ABS_MISC, 0, 9, 0, 0);

	ret = input_register_device(lpi->ls_input_dev);
	if (ret < 0) {
		pr_err("[LS][CM36682 error]%s: can not register ls input device\n",
				__func__);
		goto err_free_ls_input_device;
	}

	ret = misc_register(&lightsensor_misc);
	if (ret < 0) {
		pr_err("[LS][CM36682 error]%s: can not register ls misc device\n",
				__func__);
		goto err_unregister_ls_input_device;
	}

	return ret;

err_unregister_ls_input_device:
	input_unregister_device(lpi->ls_input_dev);
err_free_ls_input_device:
	input_free_device(lpi->ls_input_dev);
	return ret;
}

static int psensor_setup(struct CM36682_info *lpi)
{
	int ret;

	lpi->ps_input_dev = input_allocate_device();
	if (!lpi->ps_input_dev) {
		pr_err(
			"[PS][CM36682 error]%s: could not allocate ps input device\n",
			__func__);
		return -ENOMEM;
	}
	lpi->ps_input_dev->name = "CM36682-ps";
	set_bit(EV_ABS, lpi->ps_input_dev->evbit);
	input_set_abs_params(lpi->ps_input_dev, ABS_DISTANCE, 0, 1, 0, 0);

	ret = input_register_device(lpi->ps_input_dev);
	if (ret < 0) {
		pr_err(
			"[PS][CM36682 error]%s: could not register ps input device\n",
			__func__);
		goto err_free_ps_input_device;
	}

	ret = misc_register(&psensor_misc);
	if (ret < 0) {
		pr_err(
			"[PS][CM36682 error]%s: could not register ps misc device\n",
			__func__);
		goto err_unregister_ps_input_device;
	}

	return ret;

err_unregister_ps_input_device:
	input_unregister_device(lpi->ps_input_dev);
err_free_ps_input_device:
	input_free_device(lpi->ps_input_dev);
	return ret;
}


static int initial_CM36682(struct CM36682_info *lpi)
{
	int val, ret;
	uint16_t idReg;

	val = gpio_get_value(lpi->intr_pin);
	D("[PS][CM36682] %s, INTERRUPT GPIO val = %d\n", __func__, val);

	ret = _CM36682_I2C_Read_Word(lpi->slave_addr, ID_REG, &idReg);
	if ((ret < 0) || (idReg != 0x0083)) {
  		if (record_init_fail == 0)
  			record_init_fail = 1;
	    D("[PS][CM36682] %s, ret = %d, idReg=%d\n", __func__, ret, idReg);
  		return -ENOMEM;/*If devices without CM36682 chip and did not probe driver*/	
  }
  
	return 0;
}

static int CM36682_setup(struct CM36682_info *lpi)
{
	int ret = 0;

	als_power(1);
	msleep(5);

	ret = initial_CM36682(lpi);
	if (ret < 0) {
		pr_err(
			"[PS_ERR][CM36682 error]%s: fail to initial CM36682 (%d)\n",
			__func__, ret);
		return ret;
	}
	
	/*Default disable P sensor and L sensor*/
	ls_initial_cmd(lpi);
	psensor_initial_cmd(lpi);

	ret = request_any_context_irq(lpi->irq,
			CM36682_irq_handler,
			IRQF_TRIGGER_LOW,
			"CM36682",
			lpi);
	if (ret < 0) {
		pr_err(
			"[PS][CM36682 error]%s: req_irq(%d) fail for gpio %d (%d)\n",
			__func__, lpi->irq,
			lpi->intr_pin, ret);
	}

	return ret;
}

//#ifdef CONFIG_HAS_EARLYSUSPEND
#if 0
static void CM36682_early_suspend(struct early_suspend *h)
{
	struct CM36682_info *lpi = lp_info;

	D("[LS][CM36682] %s\n", __func__);

	if (lpi->als_enable)
		lightsensor_disable(lpi);
}

static void CM36682_late_resume(struct early_suspend *h)
{
	struct CM36682_info *lpi = lp_info;

	D("[LS][CM36682] %s\n", __func__);

	if (!lpi->als_enable)
		lightsensor_enable(lpi);
}
#endif

static void CM36682_probe_work(struct work_struct *work) {
	int ret = 0;
	struct CM36682_info *lpi = container_of(work, struct CM36682_info, probe_work);

	mutex_init(&CM36682_control_mutex);
	mutex_init(&als_enable_mutex);
	mutex_init(&als_disable_mutex);
	mutex_init(&als_get_adc_mutex);

	ret = lightsensor_setup(lpi);
	if (ret < 0) {
		pr_err("[LS][CM36682 error]%s: lightsensor_setup error!!\n",
			__func__);
		goto err_lightsensor_setup;
	}

	mutex_init(&ps_enable_mutex);
	mutex_init(&ps_disable_mutex);
	mutex_init(&ps_get_adc_mutex);

	ret = psensor_setup(lpi);
	if (ret < 0) {
		pr_err("[PS][CM36682 error]%s: psensor_setup error!!\n",
			__func__);
		goto err_psensor_setup;
	}

#if 0
	//SET LUX STEP FACTOR HERE
	// if adc raw value one step = 5/100 = 1/20 = 0.05 lux
	// the following will set the factor 0.05 = 1/20
	// and lpi->golden_adc = 1;  
	// set als_kadc = (ALS_CALIBRATED <<16) | 20;
	als_kadc = (ALS_CALIBRATED <<16) | 20;
	lpi->golden_adc = 1;
#else
	//SET LUX STEP FACTOR HERE
	// if adc raw value 1000 eqauls to 286 lux
	// the following will set the factor 286/1000
	// and lpi->golden_adc = 286;  
	// set als_kadc = (ALS_CALIBRATED <<16) | 1000;
	als_kadc = (ALS_CALIBRATED <<16) | 1000;
	lpi->golden_adc = 286;
#endif

	//ls calibrate always set to 1 
	lpi->ls_calibrate = 1;
	lpi->cal_data = 1000;

	lightsensor_set_kvalue(lpi);
	ret = lightsensor_update_table(lpi);
	if (ret < 0) {
		pr_err("[LS][CM36682 error]%s: update ls table fail\n",
			__func__);
		goto err_lightsensor_update_table;
	}

	lpi->lp_wq = create_singlethread_workqueue("CM36682_wq");
	if (!lpi->lp_wq) {
		pr_err("[PS][CM36682 error]%s: can't create workqueue\n", __func__);
		ret = -ENOMEM;
		goto err_create_singlethread_workqueue;
	}
	wake_lock_init(&(lpi->ps_wake_lock), WAKE_LOCK_SUSPEND, "proximity");

	ret = CM36682_setup(lpi);
	if (ret < 0) {
		pr_err("[PS_ERR][CM36682 error]%s: CM36682_setup error!\n", __func__);
		goto err_CM36682_setup;
	}
	lpi->CM36682_class = class_create(THIS_MODULE, "optical_sensors");
	if (IS_ERR(lpi->CM36682_class)) {
		ret = PTR_ERR(lpi->CM36682_class);
		lpi->CM36682_class = NULL;
		goto err_create_class;
	}

	lpi->ls_dev = device_create(lpi->CM36682_class,
				NULL, 0, "%s", "lightsensor");
	if (unlikely(IS_ERR(lpi->ls_dev))) {
		ret = PTR_ERR(lpi->ls_dev);
		lpi->ls_dev = NULL;
		goto err_create_ls_device;
	}

	/* register the attributes */
	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_adc);
	if (ret)
		goto err_create_ls_device_file;

	/* register the attributes */
	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_auto);
	if (ret)
		goto err_create_ls_device_file;

	/* register the attributes */
	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_kadc);
	if (ret)
		goto err_create_ls_device_file;

	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_gadc);
	if (ret)
		goto err_create_ls_device_file;

	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_adc_table);
	if (ret)
		goto err_create_ls_device_file;

	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_conf);
	if (ret)
		goto err_create_ls_device_file;

	ret = device_create_file(lpi->ls_dev, &dev_attr_ls_flevel);
	if (ret)
		goto err_create_ls_device_file;

	lpi->ps_dev = device_create(lpi->CM36682_class,
				NULL, 0, "%s", "proximity");
	if (unlikely(IS_ERR(lpi->ps_dev))) {
		ret = PTR_ERR(lpi->ps_dev);
		lpi->ps_dev = NULL;
		goto err_create_ls_device_file;
	}

	/* register the attributes */
	ret = device_create_file(lpi->ps_dev, &dev_attr_ps_adc);
	if (ret)
		goto err_create_ps_device;

	ret = device_create_file(lpi->ps_dev,
		&dev_attr_ps_parameters);
	if (ret)
		goto err_create_ps_device;

	/* register the attributes */
	ret = device_create_file(lpi->ps_dev, &dev_attr_ps_conf);
	if (ret)
		goto err_create_ps_device;

	/* register the attributes */
	ret = device_create_file(lpi->ps_dev, &dev_attr_ps_thd);
	if (ret)
		goto err_create_ps_device;

	ret = device_create_file(lpi->ps_dev, &dev_attr_ps_hw);
	if (ret)
		goto err_create_ps_device;

//#ifdef CONFIG_HAS_EARLYSUSPEND
#if 0
	lpi->early_suspend.level =
			EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	lpi->early_suspend.suspend = CM36682_early_suspend;
	lpi->early_suspend.resume = CM36682_late_resume;
	register_early_suspend(&lpi->early_suspend);
#endif

	D("[PS][CM36682] %s: Probe success!\n", __func__);
	return;

err_create_ps_device:
	device_unregister(lpi->ps_dev);
err_create_ls_device_file:
	device_unregister(lpi->ls_dev);
err_create_ls_device:
	class_destroy(lpi->CM36682_class);
err_create_class:
err_CM36682_setup:
	destroy_workqueue(lpi->lp_wq);
	wake_lock_destroy(&(lpi->ps_wake_lock));

	input_unregister_device(lpi->ls_input_dev);
	input_free_device(lpi->ls_input_dev);
	input_unregister_device(lpi->ps_input_dev);
	input_free_device(lpi->ps_input_dev);
err_create_singlethread_workqueue:
err_lightsensor_update_table:
	misc_deregister(&psensor_misc);
err_psensor_setup:
	mutex_destroy(&CM36682_control_mutex);
	mutex_destroy(&ps_enable_mutex);
	mutex_destroy(&ps_disable_mutex);
	mutex_destroy(&ps_get_adc_mutex);
	misc_deregister(&lightsensor_misc);
err_lightsensor_setup:
	mutex_destroy(&als_enable_mutex);
	mutex_destroy(&als_disable_mutex);
	mutex_destroy(&als_get_adc_mutex);
#ifdef CONFIG_OF
	kfree(lpi->pdata);
#endif
	i2c_set_clientdata(lpi->i2c_client, NULL);
	kfree(lpi);
	return;
}

static int CM36682_probe(struct i2c_client *client, const struct i2c_device_id *id) {
	int cpu, ret = 0;
	struct CM36682_info *lpi = NULL;
	struct CM36682_platform_data *pdata = NULL;

#ifdef CONFIG_LPM_MODE
    extern unsigned int poweroff_charging;
    extern unsigned int recovery_mode;
	if (1 == poweroff_charging || 1 == recovery_mode) {
        printk(KERN_ERR"%s: probe exit, lpm=%d recovery=%d\n", __func__, poweroff_charging, recovery_mode);
		return -ENODEV;
	}
#endif

	D("[PS][CM36682] %s\n", __func__);
	lpi = kzalloc(sizeof(struct CM36682_info), GFP_KERNEL);
	if (!lpi)
		return -ENOMEM;

#ifdef CONFIG_OF
	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		ret = -ENOMEM;	/* out of memory */
		goto err_platform_data_null;
	}
	ret = CM36682_parse_dt(&(client->dev), pdata);
	if (ret) {
		goto err_parse_dt;
	}
#else
	pdata = client->dev.platform_data;
	if (!pdata) {
		pr_err("[PS][CM36682 error]%s: Assign platform_data error!!\n",
			__func__);
		ret = -EBUSY;
		goto err_parse_dt;
	}
#endif

	/*D("[CM36682] %s: client->irq = %d\n", __func__, client->irq);*/
	lpi->i2c_client = client;
	lpi->irq = client->irq;
	i2c_set_clientdata(client, lpi);
	lpi->intr_pin = pdata->intr;
	lpi->adc_table = pdata->levels;
	lpi->power = pdata->power;
	lpi->slave_addr = pdata->slave_addr;
	lpi->ps_away_thd_set = pdata->ps_away_thd_set;
	lpi->ps_close_thd_set = pdata->ps_close_thd_set;	
	lpi->ps_conf1_val = pdata->ps_conf1_val;
	lpi->ps_conf3_val = pdata->ps_conf3_val;
	lpi->ls_cmd  = pdata->ls_cmd;
	lpi->record_clear_int_fail=0;
	
	D("[PS][CM36682] %s: ls_cmd 0x%x\n",
		__func__, lpi->ls_cmd);
	
	if (pdata->ls_cmd == 0) {
		lpi->ls_cmd  = CM36682_ALS_IT_160ms | CM36682_ALS_GAIN_2;
	}
	lpi->pdata = pdata;
	lp_info = lpi;

	INIT_WORK(&(lpi->probe_work), CM36682_probe_work);
    cpu = cpumask_next(smp_processor_id(), cpu_online_mask);
    if (cpu == NR_CPUS) { 
        cpu = cpumask_first(cpu_online_mask);
	}

    printk(KERN_ERR"%s: nr_cpus(%d) cur cpu(%d), netx cpu(%d)\n", __func__, NR_CPUS, smp_processor_id(), cpu);
    schedule_work_on(cpu, &lpi->probe_work);
	return 0;

err_parse_dt:
#ifdef CONFIG_OF
	kfree(pdata);
#endif
err_platform_data_null:
	kfree(lpi);

	return ret;
}
   
static int control_and_report( struct CM36682_info *lpi, uint8_t mode, uint16_t param ) {
	int ret=0;
	uint16_t adc_value = 0;
	uint16_t ps_data = 0;	

#if 0
	int i, val;
	uint32_t level = 0;
#else
	int val;
	uint16_t low_thd;
	uint32_t high_thd;
	uint32_t lux_level = 0;
	uint32_t tmp;
#endif
	
	mutex_lock(&CM36682_control_mutex);
	
	if ( mode == CONTROL_ALS ) {
		if(param){
			lpi->ls_cmd &= CM36682_ALS_SD_MASK;
		}
		else {
			lpi->ls_cmd |= CM36682_ALS_SD;
		}
		_CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);
		lpi->als_enable=param;
	}
	else if ( mode == CONTROL_PS ) {
		if (param) { 
			lpi->ps_conf1_val &= CM36682_PS_SD_MASK;
			lpi->ps_conf1_val |= CM36682_PS_INT_IN_AND_OUT;
		}
		else {
			lpi->ps_conf1_val |= CM36682_PS_SD;
			lpi->ps_conf1_val &= CM36682_PS_INT_MASK;
		}
		_CM36682_I2C_Write_Word(lpi->slave_addr, PS_CONF1, lpi->ps_conf1_val);
		lpi->ps_enable=param;
	}
	
	if ((mode == CONTROL_ALS)||(mode == CONTROL_PS)) {
		if ( param==1 ) {
			msleep(100);
		}
	}
	
	if (lpi->als_enable) {
		if ( mode == CONTROL_ALS ||
			( mode == CONTROL_INT_ISR_REPORT && 
			((param&INT_FLAG_ALS_IF_L) || (param&INT_FLAG_ALS_IF_H)))) {
		#if 0
			lpi->ls_cmd &= CM36682_ALS_INT_MASK;
			ret = _CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);  

			get_ls_adc_value(&adc_value, 0);

			if( lpi->ls_calibrate ) {
				for (i = 0; i < 10; i++) {
					if (adc_value <= (*(lpi->cali_table + i))) {
						level = i;
						if (*(lpi->cali_table + i))
							break;
					}
					if ( i == 9) {/*avoid  i = 10, because 'cali_table' of size is 10 */
						level = i;
						break;
					}
				}
			}
			else {
				for (i = 0; i < 10; i++) {
					if (adc_value <= (*(lpi->adc_table + i))) {
						level = i;
						if (*(lpi->adc_table + i))
							break;
					}
					if ( i == 9) {/*avoid  i = 10, because 'cali_table' of size is 10 */
						level = i;
						break;
					}
				}
			}

			ret = set_lsensor_range(((i == 0) || (adc_value == 0)) ? 0 :
					*(lpi->cali_table + (i - 1)) + 1, *(lpi->cali_table + i));

			lpi->ls_cmd |= CM36682_ALS_INT_EN;
			ret = _CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);  

			if ((i == 0) || (adc_value == 0)) {
				D("[LS][CM36682] %s: ADC=0x%03X, Level=%d, l_thd equal 0, h_thd = 0x%x \n",
					__func__, adc_value, level, *(lpi->cali_table + i));
			}
			else {
				D("[LS][CM36682] %s: ADC=0x%03X, Level=%d, l_thd = 0x%x, h_thd = 0x%x \n",
					__func__, adc_value, level, *(lpi->cali_table + (i - 1)) + 1, *(lpi->cali_table + i));
			}
			lpi->current_level = level;
			lpi->current_adc = adc_value;
			input_report_abs(lpi->ls_input_dev, ABS_MISC, level);
			input_sync(lpi->ls_input_dev);
		#else
			lpi->ls_cmd &= CM36682_ALS_INT_MASK;
			ret = _CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);

			get_ls_adc_value(&adc_value, 0);
			tmp = (adc_value*lpi->als_gadc)/lpi->als_kadc;
			lux_level = tmp*lpi->cal_data/1000;

			D("[LS][CM36682] %s: raw_adc=0x%04X, als_gadc=0x%04X, \
				cal_data=0x%04X, als_kadc=0x%04X, ls_calibrate = %d\n",
				__func__, adc_value, lpi->als_gadc, lpi->cal_data, lpi->als_kadc, lpi->ls_calibrate);

			/* set interrupt high/low threshold */
			low_thd = adc_value - adc_value * CHANGE_SENSITIVITY /100;
			high_thd = adc_value + adc_value * CHANGE_SENSITIVITY /100;
			if (high_thd > 65535) {
				high_thd = 65535;
			}
			ret = set_lsensor_range(low_thd, (uint16_t)high_thd);
			D("[CM36682] %s: ADC=0x%04X, Lux Level=%d, l_thd = 0x%x, h_thd = 0x%x \n",
				__func__, adc_value, lux_level, low_thd, high_thd);

			lpi->ls_cmd |= CM36682_ALS_INT_EN;
			ret = _CM36682_I2C_Write_Word(lpi->slave_addr, ALS_CONF, lpi->ls_cmd);

			lpi->current_level = lux_level;
			lpi->current_adc = adc_value;  
			input_report_abs(lpi->ls_input_dev, ABS_MISC, lux_level);
			input_sync(lpi->ls_input_dev);
		#endif
		}
	}

#define PS_CLOSE 1
#define PS_AWAY  (1<<1)
#define PS_CLOSE_AND_AWAY PS_CLOSE+PS_AWAY

  if(lpi->ps_enable){
    int ps_status = 0;
    if( mode == CONTROL_PS )
      ps_status = PS_CLOSE_AND_AWAY;
    else if(mode == CONTROL_INT_ISR_REPORT ){
      if ( param & INT_FLAG_PS_IF_CLOSE )
        ps_status |= PS_CLOSE;
      if ( param & INT_FLAG_PS_IF_AWAY )
        ps_status |= PS_AWAY;
    }

    if (ps_status!=0){
      switch(ps_status){
        case PS_CLOSE_AND_AWAY:
          get_stable_ps_adc_value(&ps_data);
          val = (ps_data >= lpi->ps_close_thd_set) ? PS_CLOSE_VAL : PS_AWAY_VAL;
          break;
        case PS_AWAY:
          val = PS_AWAY_VAL;
          break;
        case PS_CLOSE:
          val = PS_CLOSE_VAL;
          break;
        };

    get_stable_ps_adc_value(&ps_data);
	printk("%s: ps_adc = %d\n", __func__, ps_data);
      input_report_abs(lpi->ps_input_dev, ABS_DISTANCE, val);
      input_sync(lpi->ps_input_dev);        
    }
  }

  mutex_unlock(&CM36682_control_mutex);
  return ret;
}

#ifdef CONFIG_OF
static struct of_device_id CM36682_match_table[] = {
	{ .compatible = CM36682_I2C_NAME,},
	{ },
};
#endif

static const struct i2c_device_id CM36682_i2c_id[] = {
	{CM36682_I2C_NAME, 0},
	{}
};

static struct i2c_driver CM36682_driver = {
	.id_table = CM36682_i2c_id,
	.probe = CM36682_probe,
	.driver = {
		.name = CM36682_I2C_NAME,
		.owner = THIS_MODULE,
	#ifdef CONFIG_OF
		.of_match_table = CM36682_match_table,
    #endif
	},
};

static int __init CM36682_init(void)
{
	return i2c_add_driver(&CM36682_driver);
}

static void __exit CM36682_exit(void)
{
	i2c_del_driver(&CM36682_driver);
}

module_init(CM36682_init);
module_exit(CM36682_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CM36682 Driver");
