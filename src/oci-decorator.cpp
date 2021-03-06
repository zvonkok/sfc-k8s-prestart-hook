
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mount.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <yajl/yajl_tree.h>
#include <ctype.h>

#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <regex>

#include <sys/types.h>
#include <dirent.h>

#include <libconfig.h++>

#define _cleanup_(x) __attribute__((cleanup(x)))

static const std::string oci_decorator_conf = "/etc/oci-decorator/oci-decorator.d/";

static int log_level=LOG_DEBUG;

static inline void closep(int *fd) {
	if (*fd >= 0) {	close(*fd); }
	*fd = -1;
}

static inline void fclosep(FILE **fp) {
	if (*fp) { fclose(*fp); }
	*fp = NULL;
}

static inline void closedirp(DIR **dir) {
	if (*dir) { closedir(*dir); }
	*dir = NULL;
}

#define _cleanup_close_ _cleanup_(closep)
#define _cleanup_dir_ _cleanup_(closedirp)
#define _cleanup_fclose_ _cleanup_(fclosep)

#define DEFINE_CLEANUP_FUNC(type, func)	      \
	static inline void func##p(type *p) { \
		if (*p)                       \
			func(*p);             \
 	}                                     \

DEFINE_CLEANUP_FUNC(yajl_val, yajl_tree_free)

#define pr_perror(fmt, ...)   syslog(LOG_ERR,     "onload-hook <error>:   " fmt ": %m\n", ##__VA_ARGS__)
#define pr_pinfo(fmt, ...)    syslog(LOG_INFO,    "onload-hook <info>:    " fmt "\n", ##__VA_ARGS__)
#define pr_pwarning(fmt, ...) syslog(LOG_WARNING, "onload-hook <warning>: " fmt "\n", ##__VA_ARGS__)
#define pr_pdebug(fmt, ...)   syslog(LOG_DEBUG,    "onload-hook <debug>:  " fmt "\n", ##__VA_ARGS__)

#define BUFLEN 1024
#define CHUNKSIZE 4096

using namespace std;
using namespace libconfig;

string shortid(const string & id)
{
	return id.substr(0, 12);
}

void read_directory(const string & name, vector<string> & v)
{
       _cleanup_dir_ DIR* dirp = opendir(name.c_str());
        struct dirent * dp;
        while ((dp = readdir(dirp)) != NULL) {
                v.push_back(dp->d_name);
        }
}


static int cp(const string & dst, const string & src)
{
        ifstream f_src(src.c_str(), ios::binary);
        ofstream f_dst(dst.c_str(), ios::binary);

        f_dst << f_src.rdbuf();
}

static int zcopy(const string & dst, const string & src, const string & rootfs)
{
        const string file_dst = rootfs + dst;
        
        pr_pdebug("Copying %s file to: %s", dst.c_str(), file_dst.c_str());
        if (cp(file_dst, src) == -1) {
                pr_perror("Failed to copy file src=%s dst=%s", src.c_str(), file_dst.c_str());
                return 0;
        }


	return 0;
}

static int zmkdir(const string & dst, const string & rootfs)
{
        
        const string file_dst = rootfs + dst;
        struct stat stat_struct;
	
	
	if (stat(file_dst.c_str(), &stat_struct) == 0 && S_ISDIR(stat_struct.st_mode)) {
		pr_pdebug("Directory exists, skipping: %s", file_dst.c_str());
	} else {
		pr_pdebug("Creating directory: %s", file_dst.c_str());
		if (mkdir(file_dst.c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == -1) {
			pr_perror("Failed to create directory: %s", file_dst.c_str());
			return 0;
		}
	}
	
	return 0;
}

static int zmknod(const string & dev, const int32_t major, const int32_t minor, const string & rootfs)
{
        const string file_dst = rootfs + dev;
        
	struct stat stat_struct;
        
	dev_t mdev = makedev(major, minor);
	
	if (stat(file_dst.c_str(), &stat_struct) == 0) {
		pr_pdebug("Device exists, skipping: %s", file_dst.c_str());
	} else {
		pr_pdebug("Creating device file: %s", file_dst.c_str());
		if (mknod(file_dst.c_str(), S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH, mdev) == -1) {
			pr_perror("%s: Failed to create device file %s", dev.c_str());
			return 0;
		}
	}

	return 0;
}

static int zchmod(const string & path, const mode_t mode, const string & rootfs)
{
        const string file_dst = rootfs + path;
	chmod(file_dst.c_str(), mode);
}

static int32_t get_major_num(string device)
{
        string line;
        ifstream pd("/proc/devices");

        while (getline(pd, line)) {

                size_t found = line.find(device);
                
                if (found != string::npos) {
                        istringstream iss(line);
                        vector<string> results((istream_iterator<string>(iss)),
                                                         istream_iterator<string>());

                        /* skip if the device is a prefix of another device */
                        if (device.size() != results[1].size()) {
                                continue;
                        }
                        
                        pr_pdebug("major %s device %s", results[0].c_str(), results[1].c_str());
                        return stoi(results[0]);
                }
                        

        }
        return -1;
}


static string get_device_cgroup(const int32_t pid)
{
        string line;
        stringstream proc_pid_cgroup;

        proc_pid_cgroup << "/proc/" << pid << "/cgroup";
        
        ifstream ppc(proc_pid_cgroup.str().c_str());

        while (getline(ppc, line)) {
                size_t found = line.find("devices");

                if (found != string::npos) {
                        string token;
                        stringstream iss(line);
                        vector<string> cgroup;
                        
                        while (getline(iss, token, ':')) { cgroup.push_back(token); }

                        return cgroup[2];
                }
        }
   
}



static int32_t prestart(const string & id,
                        const string & rootfs,
                        int32_t pid)
{
	pr_pdebug("prestart container_id:%s rootfs:%s", id.c_str(), rootfs.c_str());
        
        _cleanup_close_  int fd = -1;

        stringstream  proc_ns_mnt;
        proc_ns_mnt << "/proc/" << pid << "/ns/mnt";
        
        fd = open(proc_ns_mnt.str().c_str(), O_RDONLY);
	if (fd < 0) {
		pr_perror("%s: Failed to open mnt namespace fd %s", id.c_str(), proc_ns_mnt.str().c_str());
		return EXIT_FAILURE;
	}
	/* Join the mount namespace of the target process */
	if (setns(fd, 0) == -1) {
		pr_perror("%s: Failed to setns to %s", id.c_str(), proc_ns_mnt.str().c_str());
		return EXIT_FAILURE;
	}

  
        enum inventory {
                devices = 0,
                binaries,
                directories,
                libraries,
                miscellaneous,
                group_entries
        };

        string inventory_name[group_entries] = {
                "devices",
                "binaries",
                "directories",
                "libraries",
                "miscellaneous"
        };
         
        vector<vector<string>> _cfg(group_entries);
        
        vector<string>  conf_files;
        read_directory(oci_decorator_conf, conf_files);
                        
        Config cfg;        
        for (auto & file : conf_files) {
                if (file.compare(".") != 0 && file.compare("..") != 0) {

                        cfg.readFile((oci_decorator_conf + file).c_str());
                        string name = cfg.lookup("name");
                        
                        const Setting& root = cfg.getRoot();

                        for (int32_t inv = 0; inv < group_entries; inv++) {
                                
                                const Setting& entry = root["inventory"][inventory_name[inv]];
                                for (int i = 0; i < entry.getLength(); i++)  {
                                        _cfg[inv].push_back(entry[i]);
                                }       
                        }
                }
        }

        
        string device_cgroup = get_device_cgroup(pid);
        pr_pdebug("cgroup: %s", device_cgroup.c_str());

        for (auto & dev : _cfg[devices]) {
                string dev_path = "/dev/" + dev;
                int32_t major = get_major_num(dev);
                
                zmknod(dev_path, major, 0, rootfs);

                stringstream allow_device;
                allow_device << "cgset -r \"devices.allow=c " << major << ":0 rwm\" " << device_cgroup;
                system(allow_device.str().c_str());
                pr_pdebug("%s", allow_device.str().c_str());
        }
        
        for (auto & dir : _cfg[directories]) {
                zmkdir(dir, rootfs);
        }
        
        for (auto & bin : _cfg[binaries]) {
                zcopy(bin, bin, rootfs);
                zchmod(bin,  S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, rootfs);
        }
        for (auto & msc : _cfg[miscellaneous]) {
                zcopy(msc, msc, rootfs);
        }

        for (auto & lib : _cfg[libraries]) {
                zcopy(lib, lib, rootfs);
        }


        
	return 0;
}

/*
 * Read the entire content of stream pointed to by 'from' into a buffer in memory.
 * Return a pointer to the resulting NULL-terminated string.
 */
char *getJSONstring(FILE *from, size_t chunksize, char *msg)
{
	struct stat stat_buf;
	char *err = NULL, *JSONstring = NULL;
	size_t nbytes, bufsize;

	if (fstat(fileno(from), &stat_buf) == -1) {
		err = "fstat failed";
		goto fail;
	}

	if (S_ISREG(stat_buf.st_mode)) {
                
                pr_pdebug("reg file");
		/*
		 * If 'from' is a regular file, allocate a buffer based
		 * on the file size and read the entire content with a
		 * single fread() call.
		 */
		if (stat_buf.st_size == 0) {
			err = "is empty";
			goto fail;
		}

		bufsize = (size_t)stat_buf.st_size;

		JSONstring = (char *)malloc(bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}

		nbytes = fread((void *)JSONstring, 1, (size_t)bufsize, from);
		if (nbytes != (size_t)bufsize) {
			err = "error encountered on read";
			goto fail;
		}
	} else {
                pr_pdebug("not reg file");

		/*
		 * If 'from' is not a regular file, call fread() iteratively
		 * to read sections of 'chunksize' bytes until EOF is reached.
		 * Call realloc() during each iteration to expand the buffer
		 * as needed.
		 */
		bufsize = 0;

		for (;;) {
			JSONstring = (char *)realloc((void *)JSONstring, bufsize + chunksize);
			if (JSONstring == NULL) {
				err = "failed to allocate buffer";
				goto fail;
			}

			nbytes = fread((void *)&JSONstring[bufsize], 1, (size_t)chunksize, from);
			bufsize += nbytes;

			if (nbytes != (size_t)chunksize) {
				if (ferror(from)) {
					err = "error encountered on read";
					goto fail;
				}
				if (feof(from))
					break;
			}
		}

		if (bufsize == 0) {
			err = "is empty";
			goto fail;
		}

		JSONstring = (char *)realloc((void *)JSONstring, bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}
	}

	/* make sure the string is NULL-terminated */
	JSONstring[bufsize] = 0;
	return JSONstring;
fail:
	free(JSONstring);
	pr_perror("%s: %s", msg, err);
	return NULL;
}

static int parse_rootfs_from_bundle(const string & id, yajl_val *node_ptr, string & rootfs)
{
	yajl_val node = *node_ptr;
	char config_file_name[PATH_MAX];
	char errbuf[BUFLEN];
	char *configData;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;

	_cleanup_fclose_ FILE *fp = NULL;

	/* 'bundle' must be specified for the OCI hooks, and from there we read the configuration file */
	const char *bundle_path[] = { "bundle", (const char *)0 };
	yajl_val v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	if (!v_bundle_path) {
		const char *bundle_path[] = { "bundlePath", (const char *)0 };
		v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	}

	if (v_bundle_path) {
		snprintf(config_file_name, PATH_MAX, "%s/config.json", YAJL_GET_STRING(v_bundle_path));
		fp = fopen(config_file_name, "r");
	} else {
		char msg[] = "bundle not found in state";
		snprintf(config_file_name, PATH_MAX, "%s", msg);
	}

	if (fp == NULL) {
		pr_perror("%s: Failed to open config file: %s", id.c_str(), config_file_name);
		return EXIT_FAILURE;
	}

	/* Read the entire config file */
	snprintf(errbuf, BUFLEN, "failed to read config data from %s", config_file_name);
	configData = getJSONstring(fp, (size_t)CHUNKSIZE, errbuf);
	if (configData == NULL)
		return EXIT_FAILURE;

	/* Parse the config file */
	memset(errbuf, 0, BUFLEN);
	config_node = yajl_tree_parse((const char *)configData, errbuf, sizeof(errbuf));
	if (config_node == NULL) {
		if (strlen(errbuf)) {
			pr_perror("%s: parse error: %s: %s", id.c_str(), config_file_name, errbuf);
		} else {
			pr_perror("%s: parse error: %s: unknown error", id.c_str(), config_file_name);
		}
		return EXIT_FAILURE;
	}

	/* Extract root path from the bundle */
	const char *root_path[] = { "root", "path", (const char *)0 };
	yajl_val v_root = yajl_tree_get(config_node, root_path, yajl_t_string);
	if (!v_root) {
		pr_perror("%s: root not found in %s", id.c_str(), config_file_name);
		return EXIT_FAILURE;
	}
        string lrootfs = YAJL_GET_STRING(v_root);

	/* Prepend bundle path if the rootfs string is relative */
	if (lrootfs[0] == '/') {
		rootfs = lrootfs;
	} else {
                stringstream new_rootfs;
                new_rootfs << YAJL_GET_STRING(v_bundle_path) << "/" << lrootfs;
		rootfs = new_rootfs.str();
	}


	return 0;
}

int main(int argc, char *argv[])
{
	_cleanup_(yajl_tree_freep) yajl_val node = NULL;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;
        
	char errbuf[BUFLEN];
	char *stateData;
	_cleanup_fclose_ FILE *fp = NULL;
        string id;

	setlogmask(LOG_UPTO(log_level));

	/* Read the entire state from stdin */
	snprintf(errbuf, BUFLEN, "failed to read state data from standard input");
	stateData = getJSONstring(stdin, (size_t)CHUNKSIZE, errbuf);
	if (stateData == NULL)
		return EXIT_FAILURE;



        memset(errbuf, 0, BUFLEN);
	node = yajl_tree_parse((const char *)stateData, errbuf, sizeof(errbuf));
	if (node == NULL) {
		if (strlen(errbuf)) {
			pr_perror("parse_error: %s", errbuf);
		} else {
			pr_perror("parse_error: unknown error");
		}
		return EXIT_FAILURE;
	}
        
	const char *id_path[] = { "id", (const char *) 0 };
	yajl_val v_id = yajl_tree_get(node, id_path, yajl_t_string);
	if (!v_id) {
		pr_perror("id not found in state");
		return EXIT_FAILURE;
	}
        
	const string container_id = YAJL_GET_STRING(v_id);
	id = shortid(container_id).c_str();

        
	const char *pid_path[] = { "pid", (const char *) 0 };
	yajl_val v_pid = yajl_tree_get(node, pid_path, yajl_t_number);
	if (!v_pid) {
		pr_perror("%s: pid not found in state", id.c_str());
		return EXIT_FAILURE;
	}


        
        int target_pid = YAJL_GET_INTEGER(v_pid);

	pr_pdebug("pid %d", target_pid);

	/* OCI hooks set target_pid to 0 on poststop, as the container process
	   already exited.  If target_pid is bigger than 0 then it is a start
	   hook.
	   In most cases the calling program should set the environment variable "stage"
	   like prestart, poststart or poststop.
	   We also support passing the stage as argv[1],
	   In certain cases we also support passing of no argv[1], and no environment variable,
	   then default to prestart if the target_pid != 0, poststop if target_pid == 0.
	*/
	char *stage = getenv("stage");
        
	if (stage == NULL && argc > 2) {
		stage = argv[1];
	}

	
	if ((stage != NULL && !strcmp(stage, "prestart")) || (argc == 1 && target_pid)) {

                string rootfs; 
		if (parse_rootfs_from_bundle(id, &node, rootfs) < 0) {
                        return EXIT_FAILURE;
                }
		if (prestart(id, rootfs, target_pid) != 0) {
			return EXIT_FAILURE;
		}
                
	} else {
		pr_pdebug("%s: only runs in prestart stage, ignoring", id.c_str());
	}

	return EXIT_SUCCESS;
}
