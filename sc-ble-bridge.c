#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <linux/uhid.h>
#include <linux/hidraw.h>
#include <gio/gio.h>

#define UHID_PATH "/dev/uhid"
#define SC_FEATURE_CHARACTERISTIC "100f6c34-1735-4313-b402-38567131e5f3"
#define SC_MTU 23
#define SC_VENDOR 0x28de
#define SC_PRODUCT 0x1106
#define SC_REAL_NAME "SteamController"
#define SC_VIRTUAL_NAME "VirtualSteamController"
#define SC_HID_MAX_BUF 192

struct Device_Info{
	GDBusProxy * featureProxy;
	int sc_fd;
	int uhid_fd;
	int uhid_fd_source;
};

static gboolean verbose_flag = FALSE;
static GDBusObjectManager * manager;

static int initial_find_sc();

static void process_new_hidraw(char * path);
static int open_sc(char * path);
static void add_new_vsc(int sc_fd);
static int setup_bluetooth_info(struct Device_Info * device_info, GDBusObjectManager * manager);
static int create_middleman(struct Device_Info * device_info);
static int create(struct Device_Info * device_info);
static void destroy_device(struct Device_Info * device_info);

static int read_feature(GDBusProxy * proxy, char * data);
static int write_feature(GDBusProxy * proxy, __u8 * data, size_t dataLen);
static int uhid_write(int fd, const struct uhid_event *ev);
static int move_sc_data(struct Device_Info * device_info);
static void handle_set_report(struct Device_Info * device_info, struct uhid_event * ev);
static void handle_get_report(struct Device_Info * device_info, struct uhid_event * ev);
static int event(struct Device_Info * device_info);

static gboolean is_desired_address(const gchar * object_path, gchar * uniq_address);
static gboolean is_desired_characteristic(GDBusProxy * proxy, gchar * desired_uuid, char * uniq_address);

static gboolean inotify_fd_callback(gint fd, GIOCondition condition, gpointer user_data);
static gboolean sc_fd_callback(gint fd, GIOCondition condition, gpointer user_data);
static gboolean uhid_fd_callback(gint fd, GIOCondition condition, gpointer user_data);

static void parse_args(int argc, char ** argv);
static void print_help(char * name);

int main(int argc, char **argv)
{
	GMainLoop * loop;
	int inotify_fd;

	parse_args(argc, argv);

	manager = g_dbus_object_manager_client_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM,G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,"org.bluez","/",
		NULL,NULL,NULL,NULL,NULL);

	if(NULL == manager){
		fprintf(stderr, "Cannot connect to bluez\n");
		return EXIT_FAILURE;
	}

	inotify_fd = inotify_init1(IN_NONBLOCK);

	if(-1 == inotify_fd){
		perror("inotify_init");
		return EXIT_FAILURE;
	}

	if (inotify_add_watch(inotify_fd, "/dev", IN_CREATE | IN_DELETE) < 0) {
	   fprintf(stderr, "Cannot watch '/dev': %s\n", strerror(errno));
	   return EXIT_FAILURE;
	}

	fprintf(stderr, "Bridge started\n");

	initial_find_sc();

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_fd_add(inotify_fd, G_IO_IN, inotify_fd_callback, NULL);
	g_main_loop_run(loop);

	return EXIT_SUCCESS;
}

static int read_feature(GDBusProxy * proxy, char * data){
  GError * error = NULL;

	if(NULL == proxy)
		return 0;

	int i = 0;

  GVariant * opt = g_variant_new ("(a{sv})", NULL);
  GVariant * ret = g_dbus_proxy_call_sync(proxy, "ReadValue",
  opt, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(NULL == ret){
    g_print("%s\n", error->message);
		g_clear_error(&error);
    return 0;
  }

  GVariantIter * iter = NULL;

  g_variant_get (ret, "(ay)", &iter);
  while (g_variant_iter_loop (iter, "y", &(data[i]))){
    i++;
  }

  g_variant_iter_free (iter);

  return i;
}

static int write_feature(GDBusProxy * proxy, __u8 * data, size_t dataLen){
  GError * error = NULL;
  GVariant * vtuple;
  GVariant * vdata[2];

	if(NULL == proxy)
		return 1;

  vdata[0] = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, dataLen, sizeof(char));
  vdata[1] = g_variant_new ("a{sv}", NULL);
  vtuple = g_variant_new_tuple(vdata, 2);

  GVariant * ret = g_dbus_proxy_call_sync(proxy, "WriteValue",
  vtuple, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(NULL == ret){
		g_print("%s\n",error->message);
		g_clear_error(&error);
    return error->code;
  }

	g_variant_unref(ret);
  return 0;
}

static int uhid_write(int fd, const struct uhid_event *ev){
	ssize_t ret;

	ret = write(fd, ev, sizeof(*ev));
	if (ret < 0) {
		fprintf(stderr, "Cannot write to uhid: %m\n");
		return -errno;
	} else if (ret != sizeof(*ev)) {
		fprintf(stderr, "Wrong size written to uhid: %zd != %zu\n",
			ret, sizeof(ev));
		return -EFAULT;
	} else {
		return 0;
	}
}

static int create(struct Device_Info * device_info){
	char buf[SC_HID_MAX_BUF];
	struct hidraw_report_descriptor rpt_desc;
	int ret;
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));

	ev.type = UHID_CREATE2;
	strcpy((char *)ev.u.create2.name, SC_VIRTUAL_NAME);

	ret = ioctl(device_info->sc_fd, HIDIOCGRAWPHYS(SC_HID_MAX_BUF), buf);
	if (ret < 0){
		perror("HIDIOCGRAWPHYS");
		return 1;
	}
	strcpy((char *)ev.u.create2.phys, buf);

	ret = ioctl(device_info->sc_fd, HIDIOCGRAWUNIQ(SC_HID_MAX_BUF), buf);
	if (ret < 0){
		perror("HIDIOCGRAWUNIQ");
		return 1;
	}

	strcpy((char*)ev.u.create2.uniq, buf);

	ret = ioctl(device_info->sc_fd, HIDIOCGRDESCSIZE, &ev.u.create2.rd_size);
	if (ret < 0){
		perror("HIDIOCGRDESCSIZE");
		return 1;
	} else if(ret > SC_HID_MAX_BUF -1){
		fprintf(stderr,"HIDIOCGRDESCSIZE Too big\n");
		return 1;
	}

	/* Get Report Descriptor */
	rpt_desc.size = ev.u.create2.rd_size;
	ret = ioctl(device_info->sc_fd, HIDIOCGRDESC, &rpt_desc);
	if (ret < 0) {
		perror("HIDIOCGRDESC");
		return 1;
	}

	memcpy((char *)ev.u.create2.rd_data, rpt_desc.value, rpt_desc.size);

	ev.u.create2.bus = BUS_BLUETOOTH;
	ev.u.create2.vendor = SC_VENDOR;
	ev.u.create2.product = SC_PRODUCT;
	ev.u.create2.version = 0;
	ev.u.create2.country = 1;

	return uhid_write(device_info->uhid_fd, &ev);
}

static void destroy_device(struct Device_Info * device_info){
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));

	close(device_info->sc_fd);

	g_object_unref(G_OBJECT(device_info->featureProxy));
	device_info->featureProxy = NULL;

	ev.type = UHID_DESTROY;
	uhid_write(device_info->uhid_fd, &ev);
	close(device_info->uhid_fd);

	if(-1 != device_info->uhid_fd_source)
		g_source_remove(device_info->uhid_fd_source);

	free(device_info);
}

static int move_sc_data(struct Device_Info * device_info){
	int ret, i;
	char buf[SC_MTU];
	struct uhid_event ev;

	ret = read(device_info->sc_fd, buf, SC_MTU);

	if(ret > 0){
		if(3 == buf[0]){
			memset(&ev, 0, sizeof(ev));

			ev.type = UHID_INPUT2;
			ev.u.input2.size = ret;
			memcpy(ev.u.input2.data,buf,ret);

			if(verbose_flag){
				fprintf(stderr,"Data from SC:\n");
				for(i = 0;i < ret;i++)
					fprintf(stderr,"%hhx ",buf[i]);
				fprintf(stderr,"\n");
			}

			uhid_write(device_info->uhid_fd,&ev);
		}

		return 0;
	}

	if(EAGAIN == errno)
		return 0;

	if(0 != ret)
		perror("read");

	return 1;
}

static void handle_set_report(struct Device_Info * device_info, struct uhid_event * ev){
	int id, ret, i;

	if(verbose_flag){
		fprintf(stderr,"UHID_SET_REPORT\n");
		for(i = 0;i < ev->u.set_report.size;i++){
			fprintf(stderr,"%hhx ",ev->u.set_report.data[i]);
		}
		fprintf(stderr,"\n");
	}

	id = ev->u.set_report.id;

	ret = write_feature(device_info->featureProxy,ev->u.set_report.data+1,ev->u.set_report.size-1);

	ev->type = UHID_SET_REPORT_REPLY;
	ev->u.set_report_reply.id = id;
	ev->u.set_report_reply.err = ret;

	uhid_write(device_info->uhid_fd, ev);
}

static void handle_get_report(struct Device_Info * device_info, struct uhid_event * ev){
	int id, ret, err = 1, i;
	char buf[SC_MTU];

	id = ev->u.get_report.id;

	ret = read_feature(device_info->featureProxy, buf);

	if(verbose_flag){
		fprintf(stderr,"UHID_GET_REPORT\n");
		for(i = 0;i < ret;i++){
			fprintf(stderr,"%hhx ",buf[i]);
		}
		fprintf(stderr,"\n");
	}

	if(0 != ret){
		ret--;
		err = 0;
	}

	ev->type = UHID_GET_REPORT_REPLY;
	ev->u.get_report_reply.id = id;
	ev->u.get_report_reply.err = err;
	ev->u.get_report_reply.size = ret;
	if(ret > 0)
		memcpy(ev->u.get_report_reply.data,buf+1,ev->u.get_report_reply.size);

	uhid_write(device_info->uhid_fd, ev);
}

static int event(struct Device_Info * device_info){
	struct uhid_event ev;
	ssize_t ret;

	memset(&ev, 0, sizeof(ev));
	ret = read(device_info->uhid_fd, &ev, sizeof(ev));
	if (0 == ret) {
		fprintf(stderr, "Read HUP on uhid\n");
		return -EFAULT;
	} else if (ret < 0) {
		fprintf(stderr, "Cannot read uhid: %m\n");
		return -errno;
	} else if (ret != sizeof(ev)) {
		fprintf(stderr, "Invalid size read from uhid: %zd != %zu\n",
			ret, sizeof(ev));
		return -EFAULT;
	}

	switch (ev.type) {
	case UHID_START:
		fprintf(stderr, "UHID_START from uhid\n");
		break;
	case UHID_STOP:
		fprintf(stderr, "UHID_STOP from uhid\n");
		break;
	case UHID_OPEN:
		fprintf(stderr, "UHID_OPEN from uhid\n");
		break;
	case UHID_CLOSE:
		fprintf(stderr, "UHID_CLOSE from uhid\n");
		break;
	case UHID_OUTPUT:
		if(verbose_flag)
			fprintf(stderr, "UHID_OUTPUT from uhid\n");
		break;
	case UHID_SET_REPORT:
		handle_set_report(device_info, &ev);
		break;
	case UHID_GET_REPORT:
		handle_get_report(device_info, &ev);
		break;
	default:
		fprintf(stderr, "Unhandled event from uhid: %u\n", ev.type);
	}

	return 0;
}

static void print_help(char * name){
	printf("USAGE: %s [-v]\n", name);
	printf("Options:\n");
	printf("    -v - verbose mode; print all data from and to Steam Controller\n");
	printf("    -h - help; prints this message\n");
}

static void parse_args(int argc, char ** argv){
	char c;
	while ((c = getopt (argc, argv, "vh")) != -1)
    switch (c)
      {
      case 'v':
        verbose_flag = TRUE;
        break;
			case 'h':
			default:
				print_help(argv[0]);
        exit(EXIT_FAILURE);
      }
}

static int open_sc(char * path){
	int fd, ret;
	char buf[SC_HID_MAX_BUF];
	struct hidraw_devinfo info;

	fd = open(path, O_RDWR|O_NONBLOCK);
	if(fd < 0){
		perror("open");
		return -1;
	}

	ret = ioctl(fd, HIDIOCGRAWINFO, &info);
	if (ret < 0) {
		perror("HIDIOCGRAWINFO");
		close(fd);
		return -1;
	}

	ret = ioctl(fd, HIDIOCGRAWNAME(SC_HID_MAX_BUF), buf);
	if (ret < 0){
		perror("HIDIOCGRAWNAME");
		close(fd);
		return -1;
	}

	if(BUS_BLUETOOTH == info.bustype && SC_VENDOR == info.vendor &&
		SC_PRODUCT == info.product && 0 == strcmp(buf,SC_REAL_NAME)){
		return fd;
	}

	close(fd);

	return -1;
}

static int initial_find_sc(){
	DIR *dirp;
	struct dirent *dp;
	int sc_fd = -1;
	char path[288];
	if (NULL == (dirp = opendir("/dev"))) {
		perror("opendir");
		exit(EXIT_FAILURE);
	}
	do {
		errno = 0;
		if ((dp = readdir(dirp)) != NULL) {
			if(0 == strncmp(dp->d_name,"hidraw",6)){
				sprintf(path,"/dev/%s",dp->d_name);
				process_new_hidraw(path);
			}
		}
	} while (dp != NULL);

	if (errno != 0){
		perror("readdir");
		exit(EXIT_FAILURE);
	}
	if(closedir(dirp)){
		perror("closedir");
		exit(EXIT_FAILURE);
	}
	return sc_fd;
}

static gboolean is_desired_address(const gchar * object_path, gchar * uniq_address){
	int i, j;
	gchar path_lower_char;
	const gchar * path_iter = object_path;
	gchar * address_iter = uniq_address;

	while('_' != *path_iter && '\0' != *path_iter){
		path_iter++;
	}

	if('\0' == *path_iter)
		return FALSE;

	path_iter++;

	for(i = 0;i < 4;i++){
		for(j = 0;j < 2;j++){
			path_lower_char = g_ascii_tolower(*path_iter);
			if(path_lower_char != *address_iter || '\0' == path_lower_char || '\0' == *address_iter)
				return FALSE;
			path_iter++;
			address_iter++;
		}
		path_iter++;
		address_iter++;
	}

	return TRUE;
}

static gboolean is_desired_characteristic(GDBusProxy * proxy, gchar * desired_uuid, char * uniq_address){
	GVariant * variant = g_dbus_proxy_get_cached_property(proxy, "UUID");
	const gchar * uuid = g_variant_get_string(variant, NULL);
	g_variant_unref(variant);

	if(0 == g_ascii_strcasecmp(uuid, desired_uuid) &&
			is_desired_address(g_dbus_proxy_get_object_path(proxy), uniq_address))
		return TRUE;

	return FALSE;
}

static int setup_bluetooth_info(struct Device_Info * device_info, GDBusObjectManager * manager){
	int ret;
	char buf[SC_HID_MAX_BUF];
	GDBusObject * obj;

	device_info->featureProxy = NULL;

	ret = ioctl(device_info->sc_fd, HIDIOCGRAWUNIQ(SC_HID_MAX_BUF), buf);
	if (ret < 0){
		perror("HIDIOCGRAWUNIQ");
		return 1;
	}

	GList * list = g_dbus_object_manager_get_objects(manager);
	GList * temp = list;
	GDBusInterface * characteristic;

	GDBusProxy * proxy = NULL;
	while(NULL != temp){
		obj = temp->data;
		temp = temp->next;

		characteristic = g_dbus_object_get_interface(obj,"org.bluez.GattCharacteristic1");
		if(NULL != characteristic){
			proxy = G_DBUS_PROXY(characteristic);

			if(is_desired_characteristic(proxy, SC_FEATURE_CHARACTERISTIC, buf)){
				device_info->featureProxy = proxy;
				break;
			}

			g_object_unref(characteristic);
		}
	}

	g_list_free_full (g_steal_pointer (&list), g_object_unref);

	return NULL == device_info->featureProxy;
}

static gboolean sc_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	struct Device_Info * device_info = (struct Device_Info *)user_data;

	if(condition & G_IO_HUP){
		fprintf(stderr,"Steam Controller disconnected\n");
		destroy_device(device_info);
		return G_SOURCE_REMOVE;
	}

	if(condition & G_IO_IN){
		if(move_sc_data(device_info)){
			destroy_device(device_info);
			return G_SOURCE_REMOVE;
		}
	}

	return G_SOURCE_CONTINUE;
}

static gboolean uhid_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	struct Device_Info * device_info = (struct Device_Info *)user_data;

	if(condition & G_IO_IN){
		if(event(device_info))
			return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static void add_new_vsc(int sc_fd){
	int fd = open(UHID_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open uhid %s: %m\n", UHID_PATH);
		close(sc_fd);
		return;
	}

	struct Device_Info * device_info = (struct Device_Info *)malloc(sizeof(struct Device_Info*));

	if(NULL == device_info){
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	device_info->sc_fd = sc_fd;
	device_info->uhid_fd = fd;
	device_info->uhid_fd_source = -1;

	if(0 != create_middleman(device_info)){
		free(device_info);
	}
}

static int create_middleman(struct Device_Info * device_info){
	if(setup_bluetooth_info(device_info, manager)){
		fprintf(stderr, "Setting up bluetooth failed\n");
		return 1;
	}

	if(create(device_info))
		return 1;

	g_unix_fd_add(device_info->sc_fd, G_IO_IN | G_IO_HUP, sc_fd_callback, device_info);
	device_info->uhid_fd_source = g_unix_fd_add(device_info->uhid_fd, G_IO_IN, uhid_fd_callback, device_info);
	fprintf(stderr,"Connected to SC\n");
	return 0;
}

static void process_new_hidraw(char * path){
	int fd;

	fd = open_sc(path);

	if(-1 != fd)
		add_new_vsc(fd);
}

static gboolean inotify_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	char buf[4096]
			__attribute__ ((aligned(__alignof__(struct inotify_event))));
	char path[256];
	const struct inotify_event *event;
	ssize_t ret;

	if(condition & G_IO_IN){
		for(;;) {
	     ret = read(fd, buf, sizeof(buf));
	     if(ret == -1 && errno != EAGAIN) {
	         perror("read");
	         exit(EXIT_FAILURE);
	     }

	     if(ret <= 0)
	         break;

	     for(char *ptr = buf; ptr < buf + ret;
	             ptr += sizeof(struct inotify_event) + event->len) {

	         event = (const struct inotify_event *) ptr;
					 if(event->len <= 0 || 0 != strncmp(event->name, "hidraw", 6))
					 	continue;

	         if(event->mask & IN_CREATE){
						 sprintf(path, "/dev/%s",event->name);
						 process_new_hidraw(path);
					 }
	     }
	 }
	}

	return G_SOURCE_CONTINUE;
}
