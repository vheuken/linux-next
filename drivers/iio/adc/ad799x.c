/*
 * iio/adc/ad799x.c
 * Copyright (C) 2010-2011 Michael Hennerich, Analog Devices Inc.
 *
 * based on iio/adc/max1363
 * Copyright (C) 2008-2010 Jonathan Cameron
 *
 * based on linux/drivers/i2c/chips/max123x
 * Copyright (C) 2002-2004 Stefan Eletzhofer
 *
 * based on linux/drivers/acron/char/pcf8583.c
 * Copyright (C) 2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * ad799x.c
 *
 * Support for ad7991, ad7995, ad7999, ad7992, ad7993, ad7994, ad7997,
 * ad7998 and similar chips.
 *
 */

#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/module.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define AD799X_CHANNEL_SHIFT			4
#define AD799X_STORAGEBITS			16
/*
 * AD7991, AD7995 and AD7999 defines
 */

#define AD7991_REF_SEL				0x08
#define AD7991_FLTR				0x04
#define AD7991_BIT_TRIAL_DELAY			0x02
#define AD7991_SAMPLE_DELAY			0x01

/*
 * AD7992, AD7993, AD7994, AD7997 and AD7998 defines
 */

#define AD7998_FLTR				0x08
#define AD7998_ALERT_EN				0x04
#define AD7998_BUSY_ALERT			0x02
#define AD7998_BUSY_ALERT_POL			0x01

#define AD7998_CONV_RES_REG			0x0
#define AD7998_ALERT_STAT_REG			0x1
#define AD7998_CONF_REG				0x2
#define AD7998_CYCLE_TMR_REG			0x3

#define AD7998_DATALOW_REG(x)			((x) * 3 + 0x4)
#define AD7998_DATAHIGH_REG(x)			((x) * 3 + 0x5)
#define AD7998_HYST_REG(x)			((x) * 3 + 0x6)

#define AD7998_CYC_MASK				0x7
#define AD7998_CYC_DIS				0x0
#define AD7998_CYC_TCONF_32			0x1
#define AD7998_CYC_TCONF_64			0x2
#define AD7998_CYC_TCONF_128			0x3
#define AD7998_CYC_TCONF_256			0x4
#define AD7998_CYC_TCONF_512			0x5
#define AD7998_CYC_TCONF_1024			0x6
#define AD7998_CYC_TCONF_2048			0x7

#define AD7998_ALERT_STAT_CLEAR			0xFF

/*
 * AD7997 and AD7997 defines
 */

#define AD7997_8_READ_SINGLE			0x80
#define AD7997_8_READ_SEQUENCE			0x70
/* TODO: move this into a common header */
#define RES_MASK(bits)	((1 << (bits)) - 1)

enum {
	ad7991,
	ad7995,
	ad7999,
	ad7992,
	ad7993,
	ad7994,
	ad7997,
	ad7998
};

/**
 * struct ad799x_chip_info - chip specific information
 * @channel:		channel specification
 * @num_channels:	number of channels
 * @default_config:	device default configuration
 * @info:		pointer to iio_info struct
 */
struct ad799x_chip_info {
	struct iio_chan_spec		channel[9];
	int				num_channels;
	u16				default_config;
	const struct iio_info		*info;
};

struct ad799x_state {
	struct i2c_client		*client;
	const struct ad799x_chip_info	*chip_info;
	struct regulator		*reg;
	struct regulator		*vref;
	unsigned			id;
	u16				config;

	u8				*rx_buf;
	unsigned int			transfer_size;
};

/**
 * ad799x_trigger_handler() bh of trigger launched polling to ring buffer
 *
 * Currently there is no option in this driver to disable the saving of
 * timestamps within the ring.
 **/
static irqreturn_t ad799x_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad799x_state *st = iio_priv(indio_dev);
	int b_sent;
	u8 cmd;

	switch (st->id) {
	case ad7991:
	case ad7995:
	case ad7999:
		cmd = st->config |
			(*indio_dev->active_scan_mask << AD799X_CHANNEL_SHIFT);
		break;
	case ad7992:
	case ad7993:
	case ad7994:
		cmd = (*indio_dev->active_scan_mask << AD799X_CHANNEL_SHIFT) |
			AD7998_CONV_RES_REG;
		break;
	case ad7997:
	case ad7998:
		cmd = AD7997_8_READ_SEQUENCE | AD7998_CONV_RES_REG;
		break;
	default:
		cmd = 0;
	}

	b_sent = i2c_smbus_read_i2c_block_data(st->client,
			cmd, st->transfer_size, st->rx_buf);
	if (b_sent < 0)
		goto out;

	iio_push_to_buffers_with_timestamp(indio_dev, st->rx_buf,
			iio_get_time_ns());
out:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

/*
 * ad799x register access by I2C
 */
static int ad799x_i2c_read16(struct ad799x_state *st, u8 reg, u16 *data)
{
	struct i2c_client *client = st->client;
	int ret = 0;

	ret = i2c_smbus_read_word_swapped(client, reg);
	if (ret < 0) {
		dev_err(&client->dev, "I2C read error\n");
		return ret;
	}

	*data = (u16)ret;

	return 0;
}

static int ad799x_i2c_read8(struct ad799x_state *st, u8 reg, u8 *data)
{
	struct i2c_client *client = st->client;
	int ret = 0;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev, "I2C read error\n");
		return ret;
	}

	*data = (u8)ret;

	return 0;
}

static int ad799x_i2c_write16(struct ad799x_state *st, u8 reg, u16 data)
{
	struct i2c_client *client = st->client;
	int ret = 0;

	ret = i2c_smbus_write_word_swapped(client, reg, data);
	if (ret < 0)
		dev_err(&client->dev, "I2C write error\n");

	return ret;
}

static int ad799x_i2c_write8(struct ad799x_state *st, u8 reg, u8 data)
{
	struct i2c_client *client = st->client;
	int ret = 0;

	ret = i2c_smbus_write_byte_data(client, reg, data);
	if (ret < 0)
		dev_err(&client->dev, "I2C write error\n");

	return ret;
}

static int ad7997_8_update_scan_mode(struct iio_dev *indio_dev,
	const unsigned long *scan_mask)
{
	struct ad799x_state *st = iio_priv(indio_dev);

	kfree(st->rx_buf);
	st->rx_buf = kmalloc(indio_dev->scan_bytes, GFP_KERNEL);
	if (!st->rx_buf)
		return -ENOMEM;

	st->transfer_size = bitmap_weight(scan_mask, indio_dev->masklength) * 2;

	switch (st->id) {
	case ad7997:
	case ad7998:
		return ad799x_i2c_write16(st, AD7998_CONF_REG,
			st->config | (*scan_mask << AD799X_CHANNEL_SHIFT));
	default:
		break;
	}

	return 0;
}

static int ad799x_scan_direct(struct ad799x_state *st, unsigned ch)
{
	u16 rxbuf;
	u8 cmd;
	int ret;

	switch (st->id) {
	case ad7991:
	case ad7995:
	case ad7999:
		cmd = st->config | ((1 << ch) << AD799X_CHANNEL_SHIFT);
		break;
	case ad7992:
	case ad7993:
	case ad7994:
		cmd = (1 << ch) << AD799X_CHANNEL_SHIFT;
		break;
	case ad7997:
	case ad7998:
		cmd = (ch << AD799X_CHANNEL_SHIFT) | AD7997_8_READ_SINGLE;
		break;
	default:
		return -EINVAL;
	}

	ret = ad799x_i2c_read16(st, cmd, &rxbuf);
	if (ret < 0)
		return ret;

	return rxbuf;
}

static int ad799x_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long m)
{
	int ret;
	struct ad799x_state *st = iio_priv(indio_dev);

	switch (m) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);
		if (iio_buffer_enabled(indio_dev))
			ret = -EBUSY;
		else
			ret = ad799x_scan_direct(st, chan->scan_index);
		mutex_unlock(&indio_dev->mlock);

		if (ret < 0)
			return ret;
		*val = (ret >> chan->scan_type.shift) &
			RES_MASK(chan->scan_type.realbits);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		ret = regulator_get_voltage(st->vref);
		if (ret < 0)
			return ret;
		*val = ret / 1000;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;
	}
	return -EINVAL;
}
static const unsigned int ad7998_frequencies[] = {
	[AD7998_CYC_DIS]	= 0,
	[AD7998_CYC_TCONF_32]	= 15625,
	[AD7998_CYC_TCONF_64]	= 7812,
	[AD7998_CYC_TCONF_128]	= 3906,
	[AD7998_CYC_TCONF_512]	= 976,
	[AD7998_CYC_TCONF_1024]	= 488,
	[AD7998_CYC_TCONF_2048]	= 244,
};
static ssize_t ad799x_read_frequency(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ad799x_state *st = iio_priv(indio_dev);

	int ret;
	u8 val;
	ret = ad799x_i2c_read8(st, AD7998_CYCLE_TMR_REG, &val);
	if (ret)
		return ret;

	val &= AD7998_CYC_MASK;

	return sprintf(buf, "%u\n", ad7998_frequencies[val]);
}

static ssize_t ad799x_write_frequency(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf,
					 size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ad799x_state *st = iio_priv(indio_dev);

	long val;
	int ret, i;
	u8 t;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&indio_dev->mlock);
	ret = ad799x_i2c_read8(st, AD7998_CYCLE_TMR_REG, &t);
	if (ret)
		goto error_ret_mutex;
	/* Wipe the bits clean */
	t &= ~AD7998_CYC_MASK;

	for (i = 0; i < ARRAY_SIZE(ad7998_frequencies); i++)
		if (val == ad7998_frequencies[i])
			break;
	if (i == ARRAY_SIZE(ad7998_frequencies)) {
		ret = -EINVAL;
		goto error_ret_mutex;
	}
	t |= i;
	ret = ad799x_i2c_write8(st, AD7998_CYCLE_TMR_REG, t);

error_ret_mutex:
	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

static int ad799x_read_event_config(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir)
{
	return 1;
}

static unsigned int ad799x_threshold_reg(const struct iio_chan_spec *chan,
					 enum iio_event_direction dir,
					 enum iio_event_info info)
{
	switch (info) {
	case IIO_EV_INFO_VALUE:
		if (dir == IIO_EV_DIR_FALLING)
			return AD7998_DATALOW_REG(chan->channel);
		else
			return AD7998_DATAHIGH_REG(chan->channel);
	case IIO_EV_INFO_HYSTERESIS:
		return AD7998_HYST_REG(chan->channel);
	default:
		return -EINVAL;
	}

	return 0;
}

static int ad799x_write_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int val, int val2)
{
	int ret;
	struct ad799x_state *st = iio_priv(indio_dev);

	mutex_lock(&indio_dev->mlock);
	ret = ad799x_i2c_write16(st, ad799x_threshold_reg(chan, dir, info),
		val);
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static int ad799x_read_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int *val, int *val2)
{
	int ret;
	struct ad799x_state *st = iio_priv(indio_dev);
	u16 valin;

	mutex_lock(&indio_dev->mlock);
	ret = ad799x_i2c_read16(st, ad799x_threshold_reg(chan, dir, info),
		&valin);
	mutex_unlock(&indio_dev->mlock);
	if (ret < 0)
		return ret;
	*val = valin;

	return IIO_VAL_INT;
}

static irqreturn_t ad799x_event_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ad799x_state *st = iio_priv(private);
	u8 status;
	int i, ret;

	ret = ad799x_i2c_read8(st, AD7998_ALERT_STAT_REG, &status);
	if (ret)
		goto done;

	if (!status)
		goto done;

	ad799x_i2c_write8(st, AD7998_ALERT_STAT_REG, AD7998_ALERT_STAT_CLEAR);

	for (i = 0; i < 8; i++) {
		if (status & (1 << i))
			iio_push_event(indio_dev,
				       i & 0x1 ?
				       IIO_UNMOD_EVENT_CODE(IIO_VOLTAGE,
							    (i >> 1),
							    IIO_EV_TYPE_THRESH,
							    IIO_EV_DIR_RISING) :
				       IIO_UNMOD_EVENT_CODE(IIO_VOLTAGE,
							    (i >> 1),
							    IIO_EV_TYPE_THRESH,
							    IIO_EV_DIR_FALLING),
				       iio_get_time_ns());
	}

done:
	return IRQ_HANDLED;
}

static IIO_DEV_ATTR_SAMP_FREQ(S_IWUSR | S_IRUGO,
			      ad799x_read_frequency,
			      ad799x_write_frequency);
static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("15625 7812 3906 1953 976 488 244 0");

static struct attribute *ad799x_event_attributes[] = {
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static struct attribute_group ad799x_event_attrs_group = {
	.attrs = ad799x_event_attributes,
	.name = "events",
};

static const struct iio_info ad7991_info = {
	.read_raw = &ad799x_read_raw,
	.driver_module = THIS_MODULE,
};

static const struct iio_info ad7993_4_7_8_info = {
	.read_raw = &ad799x_read_raw,
	.event_attrs = &ad799x_event_attrs_group,
	.read_event_config = &ad799x_read_event_config,
	.read_event_value = &ad799x_read_event_value,
	.write_event_value = &ad799x_write_event_value,
	.driver_module = THIS_MODULE,
	.update_scan_mode = ad7997_8_update_scan_mode,
};

static const struct iio_event_spec ad799x_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
			BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
			BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_HYSTERESIS),
	},
};

#define _AD799X_CHANNEL(_index, _realbits, _ev_spec, _num_ev_spec) { \
	.type = IIO_VOLTAGE, \
	.indexed = 1, \
	.channel = (_index), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.scan_index = (_index), \
	.scan_type = { \
		.sign = 'u', \
		.realbits = (_realbits), \
		.storagebits = 16, \
		.shift = 12 - (_realbits), \
		.endianness = IIO_BE, \
	}, \
	.event_spec = _ev_spec, \
	.num_event_specs = _num_ev_spec, \
}

#define AD799X_CHANNEL(_index, _realbits) \
	_AD799X_CHANNEL(_index, _realbits, NULL, 0)

#define AD799X_CHANNEL_WITH_EVENTS(_index, _realbits) \
	_AD799X_CHANNEL(_index, _realbits, ad799x_events, \
		ARRAY_SIZE(ad799x_events))

static const struct ad799x_chip_info ad799x_chip_info_tbl[] = {
	[ad7991] = {
		.channel = {
			AD799X_CHANNEL(0, 12),
			AD799X_CHANNEL(1, 12),
			AD799X_CHANNEL(2, 12),
			AD799X_CHANNEL(3, 12),
			IIO_CHAN_SOFT_TIMESTAMP(4),
		},
		.num_channels = 5,
		.info = &ad7991_info,
	},
	[ad7995] = {
		.channel = {
			AD799X_CHANNEL(0, 10),
			AD799X_CHANNEL(1, 10),
			AD799X_CHANNEL(2, 10),
			AD799X_CHANNEL(3, 10),
			IIO_CHAN_SOFT_TIMESTAMP(4),
		},
		.num_channels = 5,
		.info = &ad7991_info,
	},
	[ad7999] = {
		.channel = {
			AD799X_CHANNEL(0, 8),
			AD799X_CHANNEL(1, 8),
			AD799X_CHANNEL(2, 8),
			AD799X_CHANNEL(3, 8),
			IIO_CHAN_SOFT_TIMESTAMP(4),
		},
		.num_channels = 5,
		.info = &ad7991_info,
	},
	[ad7992] = {
		.channel = {
			AD799X_CHANNEL_WITH_EVENTS(0, 12),
			AD799X_CHANNEL_WITH_EVENTS(1, 12),
			IIO_CHAN_SOFT_TIMESTAMP(3),
		},
		.num_channels = 3,
		.default_config = AD7998_ALERT_EN,
		.info = &ad7993_4_7_8_info,
	},
	[ad7993] = {
		.channel = {
			AD799X_CHANNEL_WITH_EVENTS(0, 10),
			AD799X_CHANNEL_WITH_EVENTS(1, 10),
			AD799X_CHANNEL_WITH_EVENTS(2, 10),
			AD799X_CHANNEL_WITH_EVENTS(3, 10),
			IIO_CHAN_SOFT_TIMESTAMP(4),
		},
		.num_channels = 5,
		.default_config = AD7998_ALERT_EN,
		.info = &ad7993_4_7_8_info,
	},
	[ad7994] = {
		.channel = {
			AD799X_CHANNEL_WITH_EVENTS(0, 12),
			AD799X_CHANNEL_WITH_EVENTS(1, 12),
			AD799X_CHANNEL_WITH_EVENTS(2, 12),
			AD799X_CHANNEL_WITH_EVENTS(3, 12),
			IIO_CHAN_SOFT_TIMESTAMP(4),
		},
		.num_channels = 5,
		.default_config = AD7998_ALERT_EN,
		.info = &ad7993_4_7_8_info,
	},
	[ad7997] = {
		.channel = {
			AD799X_CHANNEL_WITH_EVENTS(0, 10),
			AD799X_CHANNEL_WITH_EVENTS(1, 10),
			AD799X_CHANNEL_WITH_EVENTS(2, 10),
			AD799X_CHANNEL_WITH_EVENTS(3, 10),
			AD799X_CHANNEL(4, 10),
			AD799X_CHANNEL(5, 10),
			AD799X_CHANNEL(6, 10),
			AD799X_CHANNEL(7, 10),
			IIO_CHAN_SOFT_TIMESTAMP(8),
		},
		.num_channels = 9,
		.default_config = AD7998_ALERT_EN,
		.info = &ad7993_4_7_8_info,
	},
	[ad7998] = {
		.channel = {
			AD799X_CHANNEL_WITH_EVENTS(0, 12),
			AD799X_CHANNEL_WITH_EVENTS(1, 12),
			AD799X_CHANNEL_WITH_EVENTS(2, 12),
			AD799X_CHANNEL_WITH_EVENTS(3, 12),
			AD799X_CHANNEL(4, 12),
			AD799X_CHANNEL(5, 12),
			AD799X_CHANNEL(6, 12),
			AD799X_CHANNEL(7, 12),
			IIO_CHAN_SOFT_TIMESTAMP(8),
		},
		.num_channels = 9,
		.default_config = AD7998_ALERT_EN,
		.info = &ad7993_4_7_8_info,
	},
};

static int ad799x_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	int ret;
	struct ad799x_state *st;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	/* this is only used for device removal purposes */
	i2c_set_clientdata(client, indio_dev);

	st->id = id->driver_data;
	st->chip_info = &ad799x_chip_info_tbl[st->id];
	st->config = st->chip_info->default_config;

	/* TODO: Add pdata options for filtering and bit delay */

	st->reg = devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(st->reg))
		return PTR_ERR(st->reg);
	ret = regulator_enable(st->reg);
	if (ret)
		return ret;
	st->vref = devm_regulator_get(&client->dev, "vref");
	if (IS_ERR(st->vref)) {
		ret = PTR_ERR(st->vref);
		goto error_disable_reg;
	}
	ret = regulator_enable(st->vref);
	if (ret)
		goto error_disable_reg;

	st->client = client;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = id->name;
	indio_dev->info = st->chip_info->info;

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = st->chip_info->channel;
	indio_dev->num_channels = st->chip_info->num_channels;

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
		&ad799x_trigger_handler, NULL);
	if (ret)
		goto error_disable_vref;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev,
						client->irq,
						NULL,
						ad799x_event_handler,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						client->name,
						indio_dev);
		if (ret)
			goto error_cleanup_ring;
	}
	ret = iio_device_register(indio_dev);
	if (ret)
		goto error_cleanup_ring;

	return 0;

error_cleanup_ring:
	iio_triggered_buffer_cleanup(indio_dev);
error_disable_vref:
	regulator_disable(st->vref);
error_disable_reg:
	regulator_disable(st->reg);

	return ret;
}

static int ad799x_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ad799x_state *st = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	iio_triggered_buffer_cleanup(indio_dev);
	regulator_disable(st->vref);
	regulator_disable(st->reg);
	kfree(st->rx_buf);

	return 0;
}

static const struct i2c_device_id ad799x_id[] = {
	{ "ad7991", ad7991 },
	{ "ad7995", ad7995 },
	{ "ad7999", ad7999 },
	{ "ad7992", ad7992 },
	{ "ad7993", ad7993 },
	{ "ad7994", ad7994 },
	{ "ad7997", ad7997 },
	{ "ad7998", ad7998 },
	{}
};

MODULE_DEVICE_TABLE(i2c, ad799x_id);

static struct i2c_driver ad799x_driver = {
	.driver = {
		.name = "ad799x",
	},
	.probe = ad799x_probe,
	.remove = ad799x_remove,
	.id_table = ad799x_id,
};
module_i2c_driver(ad799x_driver);

MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("Analog Devices AD799x ADC");
MODULE_LICENSE("GPL v2");
