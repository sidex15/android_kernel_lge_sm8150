#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/hall_ic.h>
#include <linux/err.h>

#ifdef CONFIG_LGE_COVER_DISPLAY
#include <linux/lge_panel_notify.h>
#endif

struct class *hallic_class;

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct hallic_dev *hdev = (struct hallic_dev *)
		dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", hdev->state);
}

static DEVICE_ATTR(state, S_IRUGO, state_show, NULL);


#ifdef CONFIG_LGE_COVER_DISPLAY
void lge_dd_status_notify(struct hallic_dev *hdev, int state)
{
	if (!strncmp(hdev->name, "coverdisplay", strlen(hdev->name))) {
		if (hdev->state == 0) {
			pr_info("[Display][%s] LGE_PANEL_DD_NOT_CONNECTED(%d)\n", __func__, hdev->state);
			lge_panel_notifier_call_chain(LGE_PANEL_EVENT_DUAL_DISPLAY, 0, LGE_PANEL_DD_NOT_CONNECTED);
		} else {
			pr_info("[Display][%s] LGE_PANEL_DD_CONNECTED(%d)\n", __func__, hdev->state);
			lge_panel_notifier_call_chain(LGE_PANEL_EVENT_DUAL_DISPLAY, 0, LGE_PANEL_DD_CONNECTED);
		}
	}
}
EXPORT_SYMBOL(lge_dd_status_notify);
#endif

void hallic_set_state(struct hallic_dev *hdev, int state)
{
	char name_buf[40];
	char state_buf[40];
	char *read_buf;
	char *uevent_message[3];
	int offset = 0;
	int length;

	if (!hdev->dev) {
		pr_err("hall-ic: %s not registered, dropping state %d\n",
		       hdev->name ? hdev->name : "(null)", state);
		return;
	}

	if (hdev->state != state) {
		hdev->state = state;

		read_buf = (char *)get_zeroed_page(GFP_KERNEL);
		if (read_buf) {
			snprintf(name_buf, sizeof(name_buf),
				"SWITCH_NAME=%s", hdev->name);
			uevent_message[offset++] = name_buf;

			length = state_show(hdev->dev, NULL, read_buf);
			if (length > 0) {
				if (read_buf[length -1] == '\n')
					read_buf[length -1] = 0;
				snprintf(state_buf, sizeof(state_buf),
					"SWITCH_STATE=%s", read_buf);
				uevent_message[offset++] = state_buf;
			}
			uevent_message[offset] = NULL;
			kobject_uevent_env(&hdev->dev->kobj, KOBJ_CHANGE, uevent_message);
			free_page((unsigned long)read_buf);
		} else {
			pr_err("out of memory in hallic set state\n");
			kobject_uevent(&hdev->dev->kobj, KOBJ_CHANGE);

		}
	}
}

static int create_hallic_class(void)
{
	if (!hallic_class) {
		hallic_class = class_create(THIS_MODULE, "smartcover");
		if (IS_ERR(hallic_class))
			return PTR_ERR(hallic_class);
	}

	return 0;
}

int hallic_register(struct hallic_dev *hdev)
{
	int ret;
	pr_err("%s : hall_ic register + \n", __func__);
	if (!hdev) {
		pr_err("%s : hallic register failed\n", __func__);
		return -EINVAL;
	}

	hdev->dev = device_create(hallic_class, NULL,
			MKDEV(0, 1), NULL, hdev->name);
       if (IS_ERR(hdev->dev))
	       return PTR_ERR(hdev->dev);

       ret = device_create_file(hdev->dev, &dev_attr_state);
       if (ret < 0)
	       goto err1;

       dev_set_drvdata(hdev->dev, hdev);
       hdev->state = 0;
       return 0;

err1:
       device_remove_file(hdev->dev, &dev_attr_state);

       return ret;
}

void hallic_unregister(struct hallic_dev *hdev)
{
	device_remove_file(hdev->dev, &dev_attr_state);
        dev_set_drvdata(hdev->dev, NULL);
	device_destroy(hallic_class, MKDEV(0, 1));
}

static int __init hallic_class_init(void)
{
	return create_hallic_class();
}

static void __exit hallic_class_exit(void)
{
	class_destroy(hallic_class);
}

module_init(hallic_class_init);
module_exit(hallic_class_exit);

MODULE_AUTHOR("Choonmin Lee <choonmin.lee@lge.com>");
MODULE_DESCRIPTION("Hall IC class driver");
MODULE_LICENSE("GPL");

