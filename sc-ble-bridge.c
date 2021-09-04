#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <linux/uhid.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hidraw.h>
#include <gio/gio.h>
#include <glib.h>

#define HIDRAW_PREFIX "hidraw"
#define UHID_PATH "/dev/uhid"
#define SC_FEATURE_CHARACTERISTIC "100f6c34-1735-4313-b402-38567131e5f3"
#define SC_MTU 23
#define SC_VENDOR 0x28de
#define SC_PRODUCT 0x1106
#define SC_REAL_NAME "SteamController"
#define SC_VIRTUAL_NAME "VirtualSteamController"

struct Device_Info{
	char address[18];
	GDBusProxy * featureProxy;
	gsize mtu;
} device_info;

struct inotify_data {
	int * sc_fd;
	int * uhid_fd;
};

static gboolean search_state = TRUE;
static GDBusObjectManager * manager;

int readFeature(GDBusProxy * proxy, char * data){
  GError * error = NULL;

	if(NULL == proxy)
		return 0;

  GVariant * opt = g_variant_new ("(a{sv})", NULL);
	int i=0;
  GVariant * ret = g_dbus_proxy_call_sync(proxy, "ReadValue",
  opt,G_DBUS_CALL_FLAGS_NONE,-1,NULL,&error);

  if(NULL == ret){
    g_print("%s\n",error->message);
		g_clear_error(&error);
    return 0;
  }

  GVariantIter *iter=NULL;

  g_variant_get (ret, "(ay)", &iter);
  while (g_variant_iter_loop (iter, "y", &(data[i]))){
    i++;
  }

  g_variant_iter_free (iter);

  return i;
}

int verbose_flag = 0;

int writeFeature(GDBusProxy * proxy, __u8 * data, int dataLen){
  GError * error = NULL;
  GVariant *vtuple;
  GVariant *vdata[2];

	if(NULL == proxy)
		return 1;

  vdata[0] = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, dataLen, sizeof(char));
  vdata[1] = g_variant_new ("a{sv}", NULL);
  vtuple = g_variant_new_tuple(vdata, 2);

  GVariant * ret = g_dbus_proxy_call_sync(proxy, "WriteValue",
  vtuple,G_DBUS_CALL_FLAGS_NONE,-1,NULL,&error);

  if(NULL==ret){
		g_print("%s\n",error->message);
		g_clear_error(&error);
    return error->code;
  }

	g_variant_unref(ret);
  return 0;
}

static int uhid_write(int fd, const struct uhid_event *ev)
{
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

static int create(int fd, int sc_fd)
{
	char buf[192];
	struct hidraw_report_descriptor rpt_desc;
	struct hidraw_devinfo info;
	int res;

	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	strcpy((char*)ev.u.create2.name, SC_VIRTUAL_NAME);
	/* Get Physical Location */
	res = ioctl(sc_fd, HIDIOCGRAWPHYS(192), buf);
	if (res < 0){
		perror("HIDIOCGRAWPHYS");
		return 1;
	}
	strcpy((char*)ev.u.create2.phys, buf);

	/* Get Report Descriptor Size */
	res = ioctl(sc_fd, HIDIOCGRDESCSIZE, &ev.u.create2.rd_size);
	if (res < 0){
		perror("HIDIOCGRDESCSIZE");
		return 1;
	}

	/* Get Report Descriptor */
	rpt_desc.size = ev.u.create2.rd_size;
	res = ioctl(sc_fd, HIDIOCGRDESC, &rpt_desc);
	if (res < 0) {
		perror("HIDIOCGRDESC");
		return 1;
	}

	memcpy((char *)ev.u.create2.rd_data,rpt_desc.value,rpt_desc.size);

	ev.u.create2.bus = BUS_BLUETOOTH;
	ev.u.create2.vendor = SC_VENDOR;
	ev.u.create2.product = SC_PRODUCT;
	ev.u.create2.version = 0;
	ev.u.create2.country = 1;

	return uhid_write(fd, &ev);
}

static void destroy_device(int fd)
{
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));

	g_object_unref(G_OBJECT(device_info.featureProxy));
	device_info.featureProxy = NULL;
	ev.type = UHID_DESTROY;

	uhid_write(fd, &ev);

	search_state = TRUE;
}

static int move_sc_data(int fd, int sc_fd){
	int ret, i;
	char buf[64];
	struct uhid_event ev;

	ret = read(sc_fd,buf,SC_MTU);

	if(ret > 0){
		if(3 == buf[0]){
			memset(&ev, 0, sizeof(ev));

			ev.type = UHID_INPUT2;
			ev.u.input2.size = ret;
			memcpy(ev.u.input2.data,buf,ret);

			if(verbose_flag){
				fprintf(stderr,"Data from SC:\n");
				for(i=0;i<ret;i++)
					fprintf(stderr,"%hhx ",buf[i]);
				fprintf(stderr,"\n");
			}

			uhid_write(fd,&ev);
		}

		return 0;
	}

	if(EAGAIN == errno)
		return 0;

	if(0 != ret){
		perror("read");
	}

	return 1;
}

void handle_set_report(int fd, struct uhid_event * ev){
	int id,res;

	if(verbose_flag){
		fprintf(stderr,"UHID_SET_REPORT\n");
		for(int i =0;i<ev->u.set_report.size;i++){
			fprintf(stderr,"%hhx ",ev->u.set_report.data[i]);
		}
		fprintf(stderr,"\n");
	}

	id = ev->u.set_report.id;

	res = writeFeature(device_info.featureProxy,ev->u.set_report.data+1,ev->u.set_report.size-1);

	ev->type = UHID_SET_REPORT_REPLY;
	ev->u.set_report_reply.id = id;
	ev->u.set_report_reply.err = res;

	uhid_write(fd,ev);
}

void handle_get_report(int fd, struct uhid_event * ev){
	int id,res,err=1;
	char buf[SC_MTU];

	id = ev->u.get_report.id;

	res = readFeature(device_info.featureProxy,buf);

	if(verbose_flag){
		fprintf(stderr,"UHID_GET_REPORT\n");
		for(int i =0;i<res;i++){
			fprintf(stderr,"%hhx ",buf[i]);
		}
		fprintf(stderr,"\n");
	}

	if(0 != res){
		res--;
		err = 0;
	}

	ev->type = UHID_GET_REPORT_REPLY;
	ev->u.get_report_reply.id = id;
	ev->u.get_report_reply.err = err;
	ev->u.get_report_reply.size = res;
	if(res > 0)
		memcpy(ev->u.get_report_reply.data,buf+1,ev->u.get_report_reply.size);

	uhid_write(fd,ev);
}

static int event(int fd, int sc_fd)
{
	struct uhid_event ev;
	ssize_t ret;

	memset(&ev, 0, sizeof(ev));
	ret = read(fd, &ev, sizeof(ev));
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
		handle_set_report(fd,&ev);
		break;
	case UHID_GET_REPORT:
		handle_get_report(fd,&ev);
		break;
	default:
		fprintf(stderr, "Unhandled event from uhid: %u\n", ev.type);
	}

	return 0;
}

void parse_args(int argc, char ** argv, char ** hidraw_path){
	char c;
	while ((c = getopt (argc, argv, "v")) != -1)
    switch (c)
      {
      case 'v':
        verbose_flag = 1;
        break;
			default:
        exit(1);
      }
}


int open_sc(char * path){
	int fd,res;
	char buf[256];
	struct hidraw_devinfo info;

	fd = open(path, O_RDWR|O_NONBLOCK);
	if(fd < 0){
		perror("open");
		return -1;
	}

	res = ioctl(fd, HIDIOCGRAWINFO, &info);
	if (res < 0) {
		perror("HIDIOCGRAWINFO");
		close(fd);
		return -1;
	}

	res = ioctl(fd, HIDIOCGRAWNAME(256), buf);
	if (res < 0){
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

int find_sc(){
	DIR *dirp;
	struct dirent *dp;
	int sc_fd=-1;
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
				sc_fd = open_sc(path);
				if(-1 != sc_fd){ // TODO - multiple sc support
					break;
				}
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

int isCharacteristicCorrect(GDBusProxy * proxy, gchar * desired_uuid){
	GVariant * variant = g_dbus_proxy_get_cached_property(proxy, "UUID");
	const gchar * uuid = g_variant_get_string(variant,NULL);
	int res = g_ascii_strcasecmp(uuid,desired_uuid);
	g_variant_unref(variant);
	return 0==res;
}


int setup_bluetooth_info(int sc_fd, GDBusObjectManager * manager){
	int ret;
	char buf[192];
	device_info.featureProxy=NULL;
	GDBusObject * obj;

	ret = ioctl(sc_fd, HIDIOCGRAWPHYS(192), buf);
	if (ret < 0){
		perror("HIDIOCGRAWPHYS");
		return 1;
	}
	strcpy(device_info.address, buf);

	GList * list = g_dbus_object_manager_get_objects(manager);
	GList * temp = list;
	GDBusInterface * characteristic;

	GDBusProxy * proxy = NULL;
	while(NULL!=temp){
		obj = temp->data;
		temp = temp->next;

		characteristic = g_dbus_object_get_interface(obj,"org.bluez.GattCharacteristic1");
		if(NULL!=characteristic){
			proxy = G_DBUS_PROXY(characteristic);

			if(isCharacteristicCorrect(proxy,SC_FEATURE_CHARACTERISTIC)){
				device_info.featureProxy = proxy;
				break;
			}

			g_object_unref(characteristic);
		}
	}

	g_list_free_full (g_steal_pointer (&list), g_object_unref);

	return 0;
}


gboolean sc_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	int ret;
	int * uhid_fd = (int*)user_data;

	if(condition & G_IO_HUP){
		fprintf(stderr,"Steam Controller disconnected\n");
		close(fd);
		destroy_device(*uhid_fd);
		return G_SOURCE_REMOVE;
	}

	if(condition & G_IO_IN){
		ret = move_sc_data(*uhid_fd,fd);
		if (ret){
			close(fd);
			destroy_device(*uhid_fd);
			return G_SOURCE_REMOVE;
		}
	}

	return G_SOURCE_CONTINUE;
}

int create_middleman(int * uhid_fd, int * sc_fd){
	int ret;
	if(setup_bluetooth_info(*sc_fd, manager)){
		fprintf(stderr, "Setting up bluetooth failed\n");
		return 1;
	}

	ret = create(*uhid_fd, *sc_fd);
	if(ret){
		return 1;
	}

	g_unix_fd_add(*sc_fd, G_IO_IN | G_IO_HUP, sc_fd_callback, uhid_fd);
	search_state = FALSE;
	fprintf(stderr,"Connected to SC\n");
	return 0;
}

void process_new_hidraw(char * path, struct inotify_data * idata){
	int fd;

	fd = open_sc(path);

	if(-1 != fd){
		if(search_state){
			*(idata->sc_fd) = fd;

			if(0 == create_middleman(idata->uhid_fd, idata->sc_fd)){
				return;
			}

			*(idata->sc_fd) = -1;
		} else {
			// TODO - multiple sc support
		}

	}

	close(fd);
}

gboolean inotify_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	char buf[4096]
			__attribute__ ((aligned(__alignof__(struct inotify_event))));
	char path[256];
	const struct inotify_event *event;
	ssize_t ret;
	struct inotify_data * idata = (struct inotify_data *)user_data;


	if(condition & G_IO_IN){
		for (;;) {
	     ret = read(fd, buf, sizeof(buf));
	     if (ret == -1 && errno != EAGAIN) {
	         perror("read");
	         exit(EXIT_FAILURE);
	     }

	     if (ret <= 0)
	         break;

	     for (char *ptr = buf; ptr < buf + ret;
	             ptr += sizeof(struct inotify_event) + event->len) {

	         event = (const struct inotify_event *) ptr;
					 if(event->len <= 0 || 0 != strncmp(event->name,HIDRAW_PREFIX,6))
					 	continue;

	         if (event->mask & IN_CREATE){
						 printf("IN_CREATE: ");
						 sprintf(path, "/dev/%s",event->name);
						 process_new_hidraw(path, idata);
					 }
	         if (event->mask & IN_DELETE)
	             printf("IN_DELETE: ");

	         printf("/dev/%s", event->name);
	     }
	 }
	}

	return G_SOURCE_CONTINUE;
}

gboolean uhid_fd_callback(gint fd, GIOCondition condition, gpointer user_data){
	int ret;
	int * sc_fd = (int*)user_data;

	if(condition & G_IO_HUP){
		fprintf(stderr,"Received HUP on uhid\n");
		return G_SOURCE_REMOVE;
	}

	if(condition & G_IO_IN){
		ret = event(fd, *sc_fd);
		if (ret)
			return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}




int main(int argc, char **argv)
{
	GMainLoop *loop;
	int inotify_fd, fd=-1, sc_fd=-1;
	char *hidraw_path=NULL;

	struct inotify_data idata;
	idata.sc_fd = &sc_fd;
	idata.uhid_fd = &fd;

	parse_args(argc, argv, &hidraw_path);

	manager = g_dbus_object_manager_client_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM,G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,"org.bluez","/",
		NULL,NULL,NULL,NULL,NULL);

	if(NULL == manager){
		fprintf(stderr, "Cannot connect to bluez\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "Open uhid %s\n", UHID_PATH);
	fd = open(UHID_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open uhid %s: %m\n", UHID_PATH);
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

	sc_fd = find_sc();

	if(-1 != sc_fd && 0 == create_middleman(&fd, &sc_fd)){
		g_unix_fd_add(sc_fd, G_IO_IN | G_IO_HUP, sc_fd_callback, &fd);
	}

	loop = g_main_loop_new(NULL, FALSE);

	g_unix_fd_add(inotify_fd, G_IO_IN, inotify_fd_callback, &idata);
	g_unix_fd_add(fd, G_IO_IN, uhid_fd_callback, &sc_fd);


	g_main_loop_run(loop);

	fprintf(stderr, "Destroy uhid device\n");
	destroy_device(fd);
	return EXIT_SUCCESS;
}
