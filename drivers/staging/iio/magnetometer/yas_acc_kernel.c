/*
 * Copyright (c) 2013-2015 Yamaha Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include "buffer.h"
#include "iio.h"
#include "ring_sw.h"
#include "sysfs.h"
#include "trigger.h"
#include "trigger_consumer.h"
#include "yas.h"

static struct i2c_client *this_client;

enum {
	YAS_SCAN_ACCEL_X,
	YAS_SCAN_ACCEL_Y,
	YAS_SCAN_ACCEL_Z,
	YAS_SCAN_TIMESTAMP,
};
struct yas_state {
	struct mutex lock;
	spinlock_t spin_lock;
	struct yas_acc_driver acc;
	struct i2c_client *client;
	struct iio_trigger  *trig;
	struct delayed_work work;
	int16_t sampling_frequency;
	atomic_t pseudo_irq_enable;
	int32_t accel_data[3];
	int32_t calib_bias[3];
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend sus;
#endif
};

static int yas_device_open(int32_t type)
{
	return 0;
}

static int yas_device_close(int32_t type)
{
	return 0;
}

static int yas_device_write(int32_t type, uint8_t addr, const uint8_t *buf,
		int len)
{
	uint8_t tmp[2];
	if (sizeof(tmp) - 1 < len)
		return -1;
	tmp[0] = addr;
	memcpy(&tmp[1], buf, len);
	if (i2c_master_send(this_client, tmp, len + 1) < 0)
		return -1;
	return 0;
}

static int yas_device_read(int32_t type, uint8_t addr, uint8_t *buf, int len)
{
	struct i2c_msg msg[2];
	int err;
	msg[0].addr = this_client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &addr;
	msg[1].addr = this_client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;
	err = i2c_transfer(this_client->adapter, msg, 2);
	if (err != 2) {
		dev_err(&this_client->dev,
				"i2c_transfer() read error: "
				"slave_addr=%02x, reg_addr=%02x, err=%d\n",
				this_client->addr, addr, err);
		return err;
	}
	return 0;
}

static void yas_usleep(int us)
{
	usleep_range(us, us + 1000);
}

static uint32_t yas_current_time(void)
{
	return jiffies_to_msecs(jiffies);
}

static int yas_pseudo_irq_enable(struct iio_dev *indio_dev)
{
	struct yas_state *st = iio_priv(indio_dev);
	if (!atomic_cmpxchg(&st->pseudo_irq_enable, 0, 1)) {
		mutex_lock(&st->lock);
		st->acc.set_enable(1);
		mutex_unlock(&st->lock);
		schedule_delayed_work(&st->work, 0);
	}
	return 0;
}

static int yas_pseudo_irq_disable(struct iio_dev *indio_dev)
{
	struct yas_state *st = iio_priv(indio_dev);
	if (atomic_cmpxchg(&st->pseudo_irq_enable, 1, 0)) {
		cancel_delayed_work_sync(&st->work);
		mutex_lock(&st->lock);
		st->acc.set_enable(0);
		mutex_unlock(&st->lock);
	}
	return 0;
}

static int yas_set_pseudo_irq(struct iio_dev *indio_dev, int enable)
{
	if (enable)
		yas_pseudo_irq_enable(indio_dev);
	else
		yas_pseudo_irq_disable(indio_dev);
	return 0;
}

static int yas_data_rdy_trig_poll(struct iio_dev *indio_dev)
{
	struct yas_state *st = iio_priv(indio_dev);
	unsigned long flags;
	spin_lock_irqsave(&st->spin_lock, flags);
	iio_trigger_poll(st->trig, iio_get_time_ns());
	spin_unlock_irqrestore(&st->spin_lock, flags);
	return 0;
}

static irqreturn_t yas_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct iio_buffer *buffer = indio_dev->buffer;
	struct yas_state *st = iio_priv(indio_dev);
	int len = 0, i, j;
	size_t datasize = buffer->access->get_bytes_per_datum(buffer);
	int32_t *acc;

	acc = (int32_t *) kmalloc(datasize, GFP_KERNEL);
	if (acc == NULL) {
		dev_err(indio_dev->dev.parent,
				"memory alloc failed in buffer bh");
		return -ENOMEM;
	}
	if (!bitmap_empty(indio_dev->active_scan_mask, indio_dev->masklength)) {
		j = 0;
		for (i = 0; i < 3; i++) {
			if (test_bit(i, indio_dev->active_scan_mask)) {
				acc[j] = st->accel_data[i];
				j++;
			}
		}
		len = j * 4;
	}

	/* Guaranteed to be aligned with 8 byte boundary */
	if (buffer->scan_timestamp)
		*(s64 *)(((phys_addr_t)acc + len
					+ sizeof(s64) - 1) & ~(sizeof(s64) - 1))
			= pf->timestamp;
	buffer->access->store_to(buffer, (u8 *)acc, pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);
	kfree(acc);
	return IRQ_HANDLED;
}

static int yas_data_rdy_trigger_set_state(struct iio_trigger *trig,
		bool state)
{
	struct iio_dev *indio_dev = trig->private_data;
	yas_set_pseudo_irq(indio_dev, state);
	return 0;
}

static const struct iio_trigger_ops yas_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = &yas_data_rdy_trigger_set_state,
};

static int yas_probe_trigger(struct iio_dev *indio_dev)
{
	int ret;
	struct yas_state *st = iio_priv(indio_dev);
	indio_dev->pollfunc = iio_alloc_pollfunc(&iio_pollfunc_store_time,
			&yas_trigger_handler, IRQF_ONESHOT, indio_dev,
			"%s_consumer%d", indio_dev->name, indio_dev->id);
	if (indio_dev->pollfunc == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}
	st->trig = iio_allocate_trigger("%s-dev%d",
			indio_dev->name,
			indio_dev->id);
	if (!st->trig) {
		ret = -ENOMEM;
		goto error_dealloc_pollfunc;
	}
	st->trig->dev.parent = &st->client->dev;
	st->trig->ops = &yas_trigger_ops;
	st->trig->private_data = indio_dev;
	ret = iio_trigger_register(st->trig);
	if (ret)
		goto error_free_trig;
	return 0;

error_free_trig:
	iio_free_trigger(st->trig);
error_dealloc_pollfunc:
	iio_dealloc_pollfunc(indio_dev->pollfunc);
error_ret:
	return ret;
}

static void yas_remove_trigger(struct iio_dev *indio_dev)
{
	struct yas_state *st = iio_priv(indio_dev);
	iio_trigger_unregister(st->trig);
	iio_free_trigger(st->trig);
	iio_dealloc_pollfunc(indio_dev->pollfunc);
}

static const struct iio_buffer_setup_ops yas_buffer_setup_ops = {
	.preenable = &iio_sw_buffer_preenable,
	.postenable = &iio_triggered_buffer_postenable,
	.predisable = &iio_triggered_buffer_predisable,
};

static void yas_remove_buffer(struct iio_dev *indio_dev)
{
	iio_buffer_unregister(indio_dev);
	iio_sw_rb_free(indio_dev->buffer);
};

static int yas_probe_buffer(struct iio_dev *indio_dev)
{
	int ret;
	struct iio_buffer *buffer;

	buffer = iio_sw_rb_allocate(indio_dev);
	if (!buffer) {
		ret = -ENOMEM;
		goto error_ret;
	}
	buffer->scan_timestamp = true;
	indio_dev->buffer = buffer;
	indio_dev->setup_ops = &yas_buffer_setup_ops;
	indio_dev->modes |= INDIO_BUFFER_TRIGGERED;
	ret = iio_buffer_register(indio_dev, indio_dev->channels,
			indio_dev->num_channels);
	if (ret)
		goto error_free_buf;
	iio_scan_mask_set(indio_dev, indio_dev->buffer, YAS_SCAN_ACCEL_X);
	iio_scan_mask_set(indio_dev, indio_dev->buffer, YAS_SCAN_ACCEL_Y);
	iio_scan_mask_set(indio_dev, indio_dev->buffer, YAS_SCAN_ACCEL_Z);
	return 0;

error_free_buf:
	iio_sw_rb_free(indio_dev->buffer);
error_ret:
	return ret;
}

static ssize_t yas_position_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	int ret;
	mutex_lock(&st->lock);
	ret = st->acc.get_position();
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return sprintf(buf, "%d\n", ret);
}

static ssize_t yas_position_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	int ret, position;
	sscanf(buf, "%d\n", &position);
	mutex_lock(&st->lock);
	ret = st->acc.set_position(position);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return count;
}

static ssize_t yas_sampling_frequency_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	return sprintf(buf, "%d\n", st->sampling_frequency);
}

static ssize_t yas_sampling_frequency_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	int ret, data, delay;
	ret = kstrtoint(buf, 10, &data);
	if (ret)
		return ret;
	if (data <= 0)
		return -EINVAL;
	mutex_lock(&st->lock);
	st->sampling_frequency = data;
	delay = MSEC_PER_SEC / st->sampling_frequency;
	st->acc.set_delay(delay);
	mutex_unlock(&st->lock);
	return count;
}

static int yas_write_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int val,
		int val2,
		long mask)
{
	struct yas_state  *st = iio_priv(indio_dev);

	mutex_lock(&st->lock);

	switch (mask) {
	case IIO_CHAN_INFO_CALIBBIAS:
		st->calib_bias[chan->channel2 - IIO_MOD_X] = val;
		break;
	}

	mutex_unlock(&st->lock);

	return 0;
}

static int yas_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val,
		int *val2,
		long mask) {
	struct yas_state  *st = iio_priv(indio_dev);
	int ret = -EINVAL;

	if (chan->type != IIO_ACCEL)
		return -EINVAL;

	mutex_lock(&st->lock);

	switch (mask) {
	case 0:
		*val = st->accel_data[chan->channel2 - IIO_MOD_X];
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
	case IIO_CHAN_INFO_CALIBSCALE:
		/* Gain : counts / m/s^2 = 1000000 [um/s^2] */
		/* Scaling factor : 1000000 / Gain = 1 */
		*val = 0;
		*val2 = 1;
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	case IIO_CHAN_INFO_CALIBBIAS:
		*val = st->calib_bias[chan->channel2 - IIO_MOD_X];
		ret = IIO_VAL_INT;
		break;
	}

	mutex_unlock(&st->lock);

	return ret;
}

static void yas_work_func(struct work_struct *work)
{
	struct yas_data acc[1];
	struct yas_state *st =
		container_of((struct delayed_work *)work,
				struct yas_state, work);
	struct iio_dev *indio_dev = iio_priv_to_dev(st);
	uint32_t time_before, time_after;
	int32_t delay;
	int ret, i;

	time_before = jiffies_to_msecs(jiffies);
	mutex_lock(&st->lock);
	ret = st->acc.measure(acc, 1);
	if (ret == 1) {
		for (i = 0; i < 3; i++)
			st->accel_data[i] = acc[0].xyz.v[i]
				- st->calib_bias[i];
	}
	mutex_unlock(&st->lock);
	if (ret == 1)
		yas_data_rdy_trig_poll(indio_dev);
	time_after = jiffies_to_msecs(jiffies);
	delay = MSEC_PER_SEC / st->sampling_frequency
		- (time_after - time_before);
	if (delay <= 0)
		delay = 1;
	schedule_delayed_work(&st->work, msecs_to_jiffies(delay));
}

#define YAS_ACCELEROMETER_INFO_MASK			\
	(IIO_CHAN_INFO_SCALE_SHARED_BIT |		\
	 IIO_CHAN_INFO_CALIBSCALE_SEPARATE_BIT |	\
	 IIO_CHAN_INFO_CALIBBIAS_SEPARATE_BIT)

#define YAS_ACCELEROMETER_CHANNEL(axis)		\
{							\
	.type = IIO_ACCEL,				\
	.modified = 1,					\
	.channel2 = IIO_MOD_##axis,			\
	.info_mask = YAS_ACCELEROMETER_INFO_MASK,	\
	.scan_index = YAS_SCAN_ACCEL_##axis,		\
	.scan_type = IIO_ST('s', 32, 32, 0)		\
}

static const struct iio_chan_spec yas_channels[] = {
	YAS_ACCELEROMETER_CHANNEL(X),
	YAS_ACCELEROMETER_CHANNEL(Y),
	YAS_ACCELEROMETER_CHANNEL(Z),
	IIO_CHAN_SOFT_TIMESTAMP(YAS_SCAN_TIMESTAMP)
};

static IIO_DEVICE_ATTR(sampling_frequency, S_IRUSR|S_IWUSR,
		yas_sampling_frequency_show,
		yas_sampling_frequency_store, 0);
static IIO_DEVICE_ATTR(position, S_IRUSR|S_IWUSR,
		yas_position_show, yas_position_store, 0);

static struct attribute *yas_attributes[] = {
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_position.dev_attr.attr,
	NULL
};
static const struct attribute_group yas_attribute_group = {
	.attrs = yas_attributes,
};

static const struct iio_info yas_info = {
	.read_raw = &yas_read_raw,
	.write_raw = &yas_write_raw,
	.attrs = &yas_attribute_group,
	.driver_module = THIS_MODULE,
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void yas_early_suspend(struct early_suspend *h)
{
	struct yas_state *st = container_of(h,
			struct yas_state, sus);
	if (atomic_read(&st->pseudo_irq_enable)) {
		cancel_delayed_work_sync(&st->work);
		st->acc.set_enable(0);
	}
}


static void yas_late_resume(struct early_suspend *h)
{
	struct yas_state *st = container_of(h,
			struct yas_state, sus);
	if (atomic_read(&st->pseudo_irq_enable)) {
		st->acc.set_enable(1);
		schedule_delayed_work(&st->work, 0);
	}
}
#endif

static int yas_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct yas_state *st;
	struct iio_dev *indio_dev;
	int ret, i;

	this_client = i2c;
	printk("[CCI]%s: yas_kionix_accel_probe start ---\n", __FUNCTION__);

	indio_dev = iio_allocate_device(sizeof(*st));
	if (!indio_dev) {
		ret = -ENOMEM;
		goto error_ret;
	}
	i2c_set_clientdata(i2c, indio_dev);

	indio_dev->name = id->name;
	indio_dev->dev.parent = &i2c->dev;
	indio_dev->info = &yas_info;
	indio_dev->channels = yas_channels;
	indio_dev->num_channels = ARRAY_SIZE(yas_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	st = iio_priv(indio_dev);
	st->client = i2c;
	st->sampling_frequency = 20;
	st->acc.callback.device_open = yas_device_open;
	st->acc.callback.device_close = yas_device_close;
	st->acc.callback.device_read = yas_device_read;
	st->acc.callback.device_write = yas_device_write;
	st->acc.callback.usleep = yas_usleep;
	st->acc.callback.current_time = yas_current_time;
	INIT_DELAYED_WORK(&st->work, yas_work_func);
	mutex_init(&st->lock);
#ifdef CONFIG_HAS_EARLYSUSPEND
	st->sus.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	st->sus.suspend = yas_early_suspend;
	st->sus.resume = yas_late_resume;
	register_early_suspend(&st->sus);
#endif
	for (i = 0; i < 3; i++) {
		st->accel_data[i] = 0;
		st->calib_bias[i] = 0;
	}

	ret = yas_probe_buffer(indio_dev);
	if (ret)
		goto error_free_dev;
	ret = yas_probe_trigger(indio_dev);
	if (ret)
		goto error_remove_buffer;
	ret = iio_device_register(indio_dev);
	if (ret)
		goto error_remove_trigger;
	ret = yas_acc_driver_init(&st->acc);
	if (ret < 0) {
		ret = -EFAULT;
		goto error_unregister_iio;
	}
	ret = st->acc.init();
	if (ret < 0) {
		ret = -EFAULT;
		goto error_unregister_iio;
	}
	ret = st->acc.set_enable(1);
	if (ret < 0) {
		ret = -EFAULT;
		goto error_driver_term;
	}
	spin_lock_init(&st->spin_lock);
	printk("[CCI]%s: yas_kionix_accel_probe end ---\n", __FUNCTION__);
	return 0;

error_driver_term:
	st->acc.term();
error_unregister_iio:
	iio_device_unregister(indio_dev);
error_remove_trigger:
	yas_remove_trigger(indio_dev);
error_remove_buffer:
	yas_remove_buffer(indio_dev);
error_free_dev:
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&st->sus);
#endif
	iio_free_device(indio_dev);
error_ret:
	i2c_set_clientdata(i2c, NULL);
	this_client = NULL;
	return ret;
}

static int yas_remove(struct i2c_client *i2c)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(i2c);
	struct yas_state *st;
	if (indio_dev) {
		st = iio_priv(indio_dev);
#ifdef CONFIG_HAS_EARLYSUSPEND
		unregister_early_suspend(&st->sus);
#endif
		yas_pseudo_irq_disable(indio_dev);
		st->acc.term();
		iio_device_unregister(indio_dev);
		yas_remove_trigger(indio_dev);
		yas_remove_buffer(indio_dev);
		iio_free_device(indio_dev);
		this_client = NULL;
	}
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int yas_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	if (atomic_read(&st->pseudo_irq_enable)) {
		cancel_delayed_work_sync(&st->work);
		st->acc.set_enable(0);
	}
	return 0;
}

static int yas_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct yas_state *st = iio_priv(indio_dev);
	if (atomic_read(&st->pseudo_irq_enable)) {
		st->acc.set_enable(1);
		schedule_delayed_work(&st->work, 0);
	}
	return 0;
}

static SIMPLE_DEV_PM_OPS(yas_pm_ops, yas_suspend, yas_resume);
#define YAS_PM_OPS (&yas_pm_ops)
#else
#define YAS_PM_OPS NULL
#endif

static const struct i2c_device_id yas_id[] = {
	{ "kxtj2", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, yas_id);

//S [CCI]Ginger modified for MSM8226 DTS
static struct of_device_id kxtj2_of_match[] = {
		{ .compatible  = "qcom,kxtj2",},
		{ .compatible  = "kxtj2",},
		{ },
};
MODULE_DEVICE_TABLE(of, kxtj2_of_match);

//E [CCI]Ginger modified for MSM8226 DTS

static struct i2c_driver yas_driver = {
	.driver = {
		.name	= "kxtj2",
		.owner	= THIS_MODULE,
		.pm	= YAS_PM_OPS,
		.of_match_table = kxtj2_of_match,  //[CCI]Ginger modified for MSM8226 DTS
	},
	.probe		= yas_probe,
	.remove		= __devexit_p(yas_remove),
	.id_table	= yas_id,
};
static int __init yas_init(void)
{
	return i2c_add_driver(&yas_driver);
}

static void __exit yas_exit(void)
{
	i2c_del_driver(&yas_driver);
}

module_init(yas_init);
module_exit(yas_exit);

MODULE_DESCRIPTION("Yamaha Acceleration I2C driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("5.4.1024");
