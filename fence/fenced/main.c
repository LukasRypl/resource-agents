/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include "fd.h"
#include "ccs.h"
#include "copyright.cf"


/* static pthread_t recv_thread; */
static int debug;
static int quit;
static int leave_finished;
static int usr_interrupt;
char our_name[MAX_CLUSTER_MEMBER_NAME_LEN+1];


#define OPTION_STRING			("cj:f:Dn:hVS")
#define LOCKFILE_NAME			"/var/run/fenced.pid"


static void print_usage(void)
{
	printf("Usage:\n");
	printf("\n");
	printf("%s [options]\n", prog_name);
	printf("\n");
	printf("Options:\n");
	printf("\n");
	printf("  -c               All nodes are in a clean state to start\n");
	printf("  -j <secs>        Post-join fencing delay (default %d)\n",
			           DEFAULT_POST_JOIN_DELAY);
	printf("  -f <secs>        Post-fail fencing delay (default %d)\n",
				   DEFAULT_POST_FAIL_DELAY);
	printf("  -D               Enable debugging code and don't fork\n");
	printf("  -h               Print this help, then exit\n");
	printf("  -n <name>        Name of the fence domain, \"default\" if none\n");
	printf("  -V               Print program version information, then exit\n");
	printf("\n");
	printf("Command line values override those in cluster.conf.\n");
	printf("For an unbounded delay use <secs> value of -1.\n");
	printf("\n");
}

static void lockfile(void)
{
	int fd, error;
	struct flock lock;
	char buf[33];

	memset(buf, 0, 33);

	fd = open(LOCKFILE_NAME, O_CREAT|O_WRONLY,
		  S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (fd < 0)
		die("cannot open/create lock file %s", LOCKFILE_NAME);

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;

	error = fcntl(fd, F_SETLK, &lock);
	if (error)
		die("fenced is already running");

	error = ftruncate(fd, 0);
	if (error)
		die("cannot clear lock file %s", LOCKFILE_NAME);

	sprintf(buf, "%d\n", getpid());

	error = write(fd, buf, strlen(buf));
	if (error <= 0)
		die("cannot write lock file %s", LOCKFILE_NAME);
}


/* SIGUSR1 will cause this program to look for a new service event from SM
   using the GETEVENT ioctl.
 
   SIGTERM will cause this program to leave the service group cleanly; it will
   do a LEAVE ioctl, get a stop event and then exit.
   
   SIGKILL will cause the program to exit without first leaving the service
   group.  In that case the kernel will clean up and leave the service group
   (as a part of cl_release on the cluster socket). */


static void sigusr1_handler(int sig)
{
	usr_interrupt = 1;
}

static void sigterm_handler(int sig)
{
	usr_interrupt = 1;
	quit = 1;
}

#if 0
/* This thread receives messages on the cluster socket and prints them. */
static void *recv_thread_fn(void *arg)
{
	fd_t *fd = arg;
	struct iovec iov[2];
	struct msghdr msg;
	struct sockaddr_cl saddr;
	char buf[256];
	int len;
	int nodeid;

	for (;;) {
		memset(buf, 0, 256);

		msg.msg_control    = NULL;
		msg.msg_controllen = 0;
		msg.msg_iovlen     = 1;
		msg.msg_iov        = iov;
		msg.msg_name       = &saddr;
		msg.msg_flags      = 0;
		msg.msg_namelen    = sizeof(saddr);
		iov[0].iov_len     = sizeof(buf);
		iov[0].iov_base    = buf;

		len = recvmsg(fd->cl_sock, &msg, MSG_OOB);

		if (len < 0 && errno == EAGAIN)
			continue;

		if (!len || len < 0)
			continue;

		memcpy(&nodeid, &saddr.scl_csid, sizeof(int));

		if (buf[0] == CLUSTER_OOB_MSG_PORTCLOSED)
			log_debug("message: oob port-closed from nodeid %d",
				  nodeid);

		else if (buf[0] == CLUSTER_OOB_MSG_SERVICEEVENT)
			log_debug("message: oob service-event");

		else if (!strcmp(buf, "hello"))
			log_debug("message: \"%s\" from nodeid %d", buf, nodeid);

		else
			log_debug("message: unknown len %d byte0 %x nodeid %d",
				len, buf[0], nodeid);
	}
}

static void send_group_message(fd_t *fd)
{
	struct iovec iov[2];
	struct msghdr msg;
	char buf[256];
	int len;

	strcpy(buf, "hello");

	iov[0].iov_len     = strlen(buf);
	iov[0].iov_base    = buf;
	msg.msg_control    = NULL;
	msg.msg_controllen = 0;
	msg.msg_iovlen     = 1;
	msg.msg_iov        = iov;
	msg.msg_name       = NULL;
	msg.msg_flags      = O_NONBLOCK;
	msg.msg_namelen    = 0;

	len = sendmsg(fd->cl_sock, &msg, 0);
}
#endif

static void print_ev(struct cl_service_event *ev)
{
	switch (ev->type) {
	case SERVICE_EVENT_STOP:
		log_debug("stop:");
		break;
	case SERVICE_EVENT_START:
		log_debug("start:");
		break;
	case SERVICE_EVENT_FINISH:
		log_debug("finish:");
		break;
	case SERVICE_EVENT_LEAVEDONE:
		log_debug("leavedone:");
		break;
	}
	log_debug("  event_id    = %u", ev->event_id);
	log_debug("  last_stop   = %u", ev->last_stop);
	log_debug("  last_start  = %u", ev->last_start);
	log_debug("  last_finish = %u", ev->last_finish);
	log_debug("  node_count  = %u", ev->node_count);

	if (ev->type != SERVICE_EVENT_START)
		return;

	switch (ev->start_type) {
	case SERVICE_START_FAILED:
		log_debug("  start_type  = %s", "failed");
		break;
	case SERVICE_START_JOIN:
		log_debug("  start_type  = %s", "join");
		break;
	case SERVICE_START_LEAVE:
		log_debug("  start_type  = %s", "leave");
		break;
	}
}

static void print_members(int count, struct cl_cluster_node *nodes)
{
	int i;

	log_debug("members:");
	for (i = 0; i < count; i++) {
		log_debug("  nodeid = %u \"%s\"", nodes->node_id, nodes->name);
		nodes++;
	}
}

static void process_event(fd_t *fd, struct cl_service_event *ev)
{
	struct cl_cluster_nodelist nodelist;
	struct cl_cluster_node *nodes;
	int error = 0, n;

	print_ev(ev);

	if (ev->type == SERVICE_EVENT_START) {
		fd->last_start = ev->event_id;

		/* space for two extra to be sure it's not too small */
		n = ev->node_count + 2;

		FENCE_RETRY(nodes = malloc(n * sizeof(struct cl_cluster_node)),
			    nodes);
		memset(nodes, 0, n * sizeof(struct cl_cluster_node));

		nodelist.max_members = n;
		nodelist.nodes = nodes;

		error = ioctl(fd->cl_sock, SIOCCLUSTER_SERVICE_GETMEMBERS,
			      &nodelist);
		if (error < 0)
			die("process_event: service get members failed");

		print_members(ev->node_count, nodes);

		do_recovery(fd, ev, nodes);

		error = ioctl(fd->cl_sock, SIOCCLUSTER_SERVICE_STARTDONE,
			      ev->event_id);
		if (error < 0)
			log_debug("process_event: start done error");

		free(nodes);
	}

	else if (ev->type == SERVICE_EVENT_LEAVEDONE)
		leave_finished = 1;

	else if (ev->type == SERVICE_EVENT_STOP)
		fd->last_stop = fd->last_start;

	else if (ev->type == SERVICE_EVENT_FINISH) {
		fd->last_finish = ev->event_id;
		do_recovery_done(fd);
	}
}

static void process_events(fd_t *fd)
{	
	struct cl_service_event event;
	sigset_t mask, oldmask;
	int error;

	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGTERM);

	for (;;) {
		memset(&event, 0, sizeof(struct cl_service_event));

		error = ioctl(fd->cl_sock, SIOCCLUSTER_SERVICE_GETEVENT, &event);
		if (error < 0)
			die("process_events: service get event failed");

		if (!error) {
			/* Wait for a signal to arrive. */
			sigprocmask(SIG_BLOCK, &mask, &oldmask);
			while (!usr_interrupt)
				sigsuspend(&oldmask);
			sigprocmask(SIG_UNBLOCK, &mask, NULL);
			usr_interrupt = 0;
		} else
			process_event(fd, &event);

		if (quit) {
			quit = 0;
			leave_finished = 0;

			error = ioctl(fd->cl_sock, SIOCCLUSTER_SERVICE_LEAVE, 0);
			if (error < 0)
				die("process_events: service leave failed");
		}

		if (leave_finished)
			break;
	}
}

static int fence_domain_add(fd_t *fd)
{
	int cl_sock, error;

	cl_sock = socket(AF_CLUSTER, SOCK_DGRAM, CLPROTO_CLIENT);
	if (cl_sock < 0)
		die("fence_domain_add: can't create cluster socket");

	error = ioctl(cl_sock, SIOCCLUSTER_SERVICE_REGISTER, fd->name);
	if (error < 0)
		die("fence_domain_add: service register failed");

	/* FIXME: SERVICE_LEVEL_FENCE is 0 but defined in service.h */
	error = ioctl(cl_sock, SIOCCLUSTER_SERVICE_SETLEVEL, 0);
	if (error < 0)
		die("fence_domain_add: service set level failed");

	signal(SIGUSR1, sigusr1_handler);
	signal(SIGTERM, sigterm_handler);

	error = ioctl(cl_sock, SIOCCLUSTER_SERVICE_SETSIGNAL, SIGUSR1);
	if (error < 0)
		die("fence_domain_add: service set signal failed");

	error = ioctl(cl_sock, SIOCCLUSTER_SERVICE_JOIN, NULL);
	if (error < 0)
		die("fence_domain_add: service join failed");

	fd->cl_sock = cl_sock;

	/* Main loop */
	process_events(fd);

	error = ioctl(cl_sock, SIOCCLUSTER_SERVICE_UNREGISTER, NULL);
	if (error < 0)
		die("fence_domain_add: unregister failed");

	return 0;
}

static int check_ccs(fd_t *fd)
{
	char path[256];
	char *name = NULL, *str = NULL;
	int error, cd, i = 0, count = 0;


	while ((cd = ccs_connect()) < 0) {
		sleep(1);
		if (++i > 9 && !(i % 10))
			log_debug("connect to ccs error %d, "
				  "check ccsd or cluster status", cd);
	}


	/* Our own nodename must be in cluster.conf before we're allowed to
	   join the fence domain and then mount gfs; other nodes need this to
	   fence us. */

	memset(path, 0, 256);
	snprintf(path, 256, "/cluster/clusternodes/clusternode[@name=\"%s\"]",
		 our_name);

	error = ccs_get(cd, path, &str);
	if (error)
		die1("local cman node name \"%s\" not found in cluster.conf",
		     our_name);


	/* If an option was set on the command line, don't set it from ccs. */

	if (fd->comline->clean_start_opt == FALSE) {
		str = NULL;
	        memset(path, 0, 256);
	        sprintf(path, "/cluster/fence_daemon/@clean_start");

		error = ccs_get(cd, path, &str);
		if (!error)
			fd->comline->clean_start = atoi(str);
		else
			fd->comline->clean_start = DEFAULT_CLEAN_START;
		if (str)
			free(str);
	}

	if (fd->comline->post_join_delay_opt == FALSE) {
		str = NULL;
	        memset(path, 0, 256);
	        sprintf(path, "/cluster/fence_daemon/@post_join_delay");

		error = ccs_get(cd, path, &str);
		if (!error)
			fd->comline->post_join_delay = atoi(str);
		else
			fd->comline->post_join_delay = DEFAULT_POST_JOIN_DELAY;
		if (str)
			free(str);
	}

	if (fd->comline->post_fail_delay_opt == FALSE) {
		str = NULL;
	        memset(path, 0, 256);
	        sprintf(path, "/cluster/fence_daemon/@post_fail_delay");

		error = ccs_get(cd, path, &str);
		if (!error)
			fd->comline->post_fail_delay = atoi(str);
		else
			fd->comline->post_fail_delay = DEFAULT_POST_FAIL_DELAY;
		if (str)
			free(str);
	}

	if (debug)
		log_debug("delay post_join %ds post_fail %ds",
		          fd->comline->post_join_delay,
		          fd->comline->post_fail_delay);

	if (fd->comline->clean_start) {
		if (debug)
			log_debug("clean start, skipping initial nodes");
		goto out;
	}

	for (i = 1; ; i++) {
		name = NULL;
	        memset(path, 0, 256);
	        sprintf(path, "/cluster/clusternodes/clusternode[%d]/@name", i);

		error = ccs_get(cd, path, &name);
		if (error || !name)
			break;

		add_complete_node(fd, 0, strlen(name), name);
		free(name);
		count++;
	}

	if (debug)
		log_debug("added %d nodes from ccs", count);
 out:
	ccs_disconnect(cd);
	return 0;
}

static int check_cluster(fd_t *fd)
{
	struct cl_cluster_node cl_node;
	int cl_sock, active, error;

	memset(&cl_node, 0, sizeof(struct cl_cluster_node));

	cl_sock = socket(AF_CLUSTER, SOCK_DGRAM, CLPROTO_CLIENT);
	if (cl_sock < 0)
		die1("can't create cman cluster socket");

	active = ioctl(cl_sock, SIOCCLUSTER_ISACTIVE, 0);
	if (!active)
		die1("cman cluster manager is not running");

	for (;;) {
		error = ioctl(cl_sock, SIOCCLUSTER_GETNODE, &cl_node);
		if (!error)
			break;
		if (debug)
			log_debug("cman getnode failed %d", error);
		sleep(1);
	}
	memcpy(our_name, cl_node.name, strlen(cl_node.name));

	if (debug)
		log_debug("our name from cman \"%s\"", our_name);

	close(cl_sock);
	return 0;
}

static fd_t *new_fd(commandline_t *comline)
{
	int namelen = strlen(comline->name);
	fd_t *fd;

	if (namelen > MAX_NAME_LEN-1)
		die1("cluster name too long, max %d", MAX_NAME_LEN-1);

	fd = malloc(sizeof(fd_t) + MAX_NAME_LEN);
	if (!fd)
		die1("no memory");

	memset(fd, 0, sizeof(fd_t) + MAX_NAME_LEN);
	memcpy(fd->name, comline->name, namelen);
	fd->namelen = namelen;

	fd->comline = comline;
	fd->first_recovery = FALSE;
	fd->last_stop = 0;
	fd->last_start = 0;
	fd->last_finish = 0;
	fd->prev_count = 0;
	INIT_LIST_HEAD(&fd->prev);
	INIT_LIST_HEAD(&fd->victims);
	INIT_LIST_HEAD(&fd->leaving);
	INIT_LIST_HEAD(&fd->complete);

	return fd;
}

static void decode_arguments(int argc, char **argv, commandline_t *comline)
{
	int cont = TRUE;
	int optchar;

	comline->post_join_delay_opt = FALSE;
	comline->post_fail_delay_opt = FALSE;
	comline->clean_start_opt = FALSE;

	while (cont) {
		optchar = getopt(argc, argv, OPTION_STRING);

		switch (optchar) {

		case 'c':
			comline->clean_start = 1;
			comline->clean_start_opt = TRUE;
			break;

		case 'j':
			comline->post_join_delay = atoi(optarg);
			comline->post_join_delay_opt = TRUE;
			break;

		case 'f':
			comline->post_fail_delay = atoi(optarg);
			comline->post_fail_delay_opt = TRUE;
			break;

		case 'D':
			comline->debug = TRUE;
			debug = TRUE;
			break;

		case 'n':
			strncpy(comline->name, optarg, MAX_NAME_LEN);
			break;

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);
			break;

		case 'V':
			printf("fenced %s (built %s %s)\n", FENCE_RELEASE_NAME,
				 __DATE__, __TIME__);
			printf("%s\n", REDHAT_COPYRIGHT);
			exit(EXIT_SUCCESS);
			break;

		case 'S':
			/* do nothing, this is a fence_tool option that
			   we ignore when fence_tool starts us */
			break;

		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(EXIT_FAILURE);
			break;

		case EOF:
			cont = FALSE;
			break;

		default:
			die1("unknown option: %c", optchar);
			break;
		};
	}

	if (!strcmp(comline->name, ""))
		strcpy(comline->name, "default");
}

void setup_debug(void)
{
	debug_sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (debug_sock < 0)
		return;

	debug_addr.sun_family = AF_LOCAL;
	strcpy(&debug_addr.sun_path[1], FENCED_SOCK_PATH);
	debug_addrlen = sizeof(sa_family_t) + strlen(debug_addr.sun_path+1) + 1;
}

int main(int argc, char **argv)
{
	commandline_t comline;
	fd_t *fd;
	int error;

	prog_name = argv[0];

	setup_debug();

	memset(&comline, 0, sizeof(commandline_t));

	decode_arguments(argc, argv, &comline);

	fd = new_fd(&comline);

	error = check_cluster(fd);
	if (error)
		die1("check_cluster error %d", error);

	error = check_ccs(fd);
	if (error)
		die1("check_ccs error %d", error);

	if (!debug) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("main: cannot fork");
			exit(EXIT_FAILURE);
		}
		if (pid)
			exit(EXIT_SUCCESS);
		setsid();
		chdir("/");
		umask(0);
		close(0);
		close(1);
		close(2);
		openlog("fenced", LOG_PID, LOG_DAEMON);
	}

	lockfile();

	fence_domain_add(fd);

	free(fd);
	return 0;
}

char *prog_name;
int debug_sock;
char debug_buf[256];
struct sockaddr_un debug_addr;
socklen_t debug_addrlen;


