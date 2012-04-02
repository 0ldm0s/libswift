/*
 *  swift.cpp
 *  swift the multiparty transport protocol
 *
 *  Created by Victor Grishchenko on 2/15/10.
 *  Copyright 2009-2012 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include "compat.h"
#include "swift.h"
#include <cfloat>

#include <event2/http.h>
#include <event2/http_struct.h>

using namespace swift;

void usage(void)
{
    fprintf(stderr,"Usage:\n");
    fprintf(stderr,"  -h, --hash\troot Merkle hash for the transmission\n");
    fprintf(stderr,"  -f, --file\tname of file to use (root hash by default)\n");
    fprintf(stderr,"  -l, --listen\t[ip:|host:]port to listen to (default: random)\n");
    fprintf(stderr,"  -t, --tracker\t[ip:|host:]port of the tracker (default: none)\n");
    fprintf(stderr,"  -D, --debug\tfile name for debugging logs (default: stdout)\n");
    fprintf(stderr,"  -B\tdebugging logs to stdout (win32 hack)\n");
    fprintf(stderr,"  -p, --progress\treport transfer progress\n");
    fprintf(stderr,"  -g, --httpgw\t[ip:|host:]port to bind HTTP content gateway to (no default)\n");
    fprintf(stderr,"  -s, --statsgw\t[ip:|host:]port to bind HTTP stats listen socket to (no default)\n");
    fprintf(stderr,"  -c, --cmdgw\t[ip:|host:]port to bind CMD listen socket to (no default)\n");
    fprintf(stderr,"  -o, --destdir\tdirectory for saving data (default: none)\n");
    fprintf(stderr,"  -u, --uprate\tupload rate limit in KiB/s (default: unlimited)\n");
    fprintf(stderr,"  -y, --downrate\tdownload rate limit in KiB/s (default: unlimited)\n");
    fprintf(stderr,"  -w, --wait\tlimit running time, e.g. 1[DHMs] (default: infinite with -l, -g)\n");
    fprintf(stderr,"  -H, --checkpoint\tcreate checkpoint of file when complete for fast restart\n");
    fprintf(stderr,"  -z, --chunksize\tchunk size in bytes (default: %d)\n", SWIFT_DEFAULT_CHUNK_SIZE);
    fprintf(stderr,"  -i, --source\tlive source input (URL or filename or - for stdin)\n");
    fprintf(stderr,"  -e, --live\tperform live download, use with -t and -h\n");

}
#define quit(...) {usage(); fprintf(stderr,__VA_ARGS__); exit(1); }

bool InstallHTTPGateway(struct event_base *evbase,Address addr,size_t chunk_size, double *maxspeed);
bool InstallStatsGateway(struct event_base *evbase,Address addr);
bool InstallCmdGateway (struct event_base *evbase,Address cmdaddr,Address httpaddr);

bool HTTPIsSending();
bool StatsQuit();
void CmdGwUpdateDLStatesCallback();

// Local prototypes
void ReportCallback(int fd, short event, void *arg);
void EndCallback(int fd, short event, void *arg);
void LiveSourceAttemptCreate();
void LiveSourceFileTimerCallback(int fd, short event, void *arg);
void LiveSourceHTTPResponseCallback(struct evhttp_request *req, void *arg);
void LiveSourceHTTPDownloadChunkCallback(struct evhttp_request *req, void *arg);


struct event evreport, evend, evlivesource;
int file = -1;
bool file_enable_checkpoint = false;
bool file_checkpointed = false;
bool report_progress = false;
bool httpgw_enabled=false,cmdgw_enabled=false;
// Gertjan fix
bool do_nat_test = false;
size_t chunk_size = SWIFT_DEFAULT_CHUNK_SIZE;

// LIVE
char* livesource_input = 0;
LiveTransfer *livesource_lt = NULL;
int livesource_fd=-1;
struct evbuffer *livesource_evb = NULL;

int main (int argc, char** argv)
{
    static struct option long_options[] =
    {
        {"hash",    required_argument, 0, 'h'},
        {"file",    required_argument, 0, 'f'},
        {"daemon",  no_argument, 0, 'd'},
        {"listen",  required_argument, 0, 'l'},
        {"tracker", required_argument, 0, 't'},
        {"debug",   no_argument, 0, 'D'},
        {"progress",no_argument, 0, 'p'},
        {"httpgw",  required_argument, 0, 'g'},
        {"wait",    optional_argument, 0, 'w'},
        {"nat-test",no_argument, 0, 'N'},
        {"statsgw", required_argument, 0, 's'}, // SWIFTPROC
        {"cmdgw",   required_argument, 0, 'c'}, // SWIFTPROC
        {"destdir", required_argument, 0, 'o'}, // SWIFTPROC
        {"uprate",  required_argument, 0, 'u'}, // RATELIMIT
        {"downrate",required_argument, 0, 'y'}, // RATELIMIT
        {"checkpoint",no_argument, 0, 'H'},
        {"chunksize",required_argument, 0, 'z'}, // CHUNKSIZE
        {"source",required_argument, 0, 'i'}, // LIVE
        {"live",no_argument, 0, 'e'}, // LIVE
        {0, 0, 0, 0}
    };

    Sha1Hash root_hash;
    char* filename = 0;
    const char *destdir = 0; // UNICODE?
    bool daemonize = false, livestream=false;
    Address bindaddr;
    Address tracker;
    Address httpaddr;
    Address statsaddr;
    Address cmdaddr;
    tint wait_time = 0;
    double maxspeed[2] = {DBL_MAX,DBL_MAX};

    LibraryInit();
    Channel::evbase = event_base_new();

    int c,n;
    while ( -1 != (c = getopt_long (argc, argv, ":h:f:dl:t:D:pg:s:c:o:u:y:z:i:wBNHe", long_options, 0)) ) {
        switch (c) {
            case 'h':
                if (strlen(optarg)!=40)
                    quit("SHA1 hash must be 40 hex symbols\n");
                root_hash = Sha1Hash(true,optarg); // FIXME ambiguity
                if (root_hash==Sha1Hash::ZERO)
                    quit("SHA1 hash must be 40 hex symbols\n");
                break;
            case 'f':
                filename = strdup(optarg);
                break;
            case 'd':
                daemonize = true;
                break;
            case 'l':
                bindaddr = Address(optarg);
                if (bindaddr==Address())
                    quit("address must be hostname:port, ip:port or just port\n");
                wait_time = TINT_NEVER;
                break;
            case 't':
                tracker = Address(optarg);
                if (tracker==Address())
                    quit("address must be hostname:port, ip:port or just port\n");
                SetTracker(tracker);
                break;
            case 'D':
                Channel::debug_file = optarg ? fopen(optarg,"a") : stderr;
                break;
            // Arno hack: get opt diff Win32 doesn't allow -D without arg
            case 'B':
            	fprintf(stderr,"SETTING DEBUG TO STDOUT\n");
                Channel::debug_file = stderr;
                break;
            case 'p':
                report_progress = true;
                break;
            case 'g':
            	httpgw_enabled = true;
                httpaddr = Address(optarg);
                if (wait_time==-1)
                    wait_time = TINT_NEVER; // seed
                break;
            case 'w':
                if (optarg) {
                    char unit = 'u';
                    if (sscanf(optarg,"%lli%c",&wait_time,&unit)!=2)
                        quit("time format: 1234[umsMHD], e.g. 1M = one minute\n");

                    switch (unit) {
                        case 'D': wait_time *= 24;
                        case 'H': wait_time *= 60;
                        case 'M': wait_time *= 60;
                        case 's': wait_time *= 1000;
                        case 'm': wait_time *= 1000;
                        case 'u': break;
                        default:  quit("time format: 1234[umsMHD], e.g. 1D = one day\n");
                    }
                } else
                    wait_time = TINT_NEVER;
                break;
            case 'N': // Gertjan fix
                do_nat_test = true;
                break;
            case 's': // SWIFTPROC
                statsaddr = Address(optarg);
                if (statsaddr==Address())
                    quit("address must be hostname:port, ip:port or just port\n");
                break;
            case 'c': // SWIFTPROC
            	cmdgw_enabled = true;
                cmdaddr = Address(optarg);
                if (cmdaddr==Address())
                    quit("address must be hostname:port, ip:port or just port\n");
                break;
            case 'o': // SWIFTPROC
                destdir = strdup(optarg); // UNICODE
                break;
            case 'u': // RATELIMIT
            	n = sscanf(optarg,"%lf",&maxspeed[DDIR_UPLOAD]);
            	if (n != 1)
            		quit("uprate must be KiB/s as float\n");
            	maxspeed[DDIR_UPLOAD] *= 1024.0;
            	break;
            case 'y': // RATELIMIT
            	n = sscanf(optarg,"%lf",&maxspeed[DDIR_DOWNLOAD]);
            	if (n != 1)
            		quit("downrate must be KiB/s as float\n");
            	maxspeed[DDIR_DOWNLOAD] *= 1024.0;
            	break;
            case 'H': //CHECKPOINT
                file_enable_checkpoint = true;
                break;
            case 'z': // CHUNKSIZE
            	n = sscanf(optarg,"%i",&chunk_size);
            	if (n != 1)
            		quit("chunk size must be bytes as int\n");
            	break;
            case 'i': // LIVE
                livesource_input = strdup(optarg);
                wait_time = TINT_NEVER;
                break;
            case 'e': // LIVE
                livestream = true;
                break;
        }

    }   // arguments parsed


    if (httpgw_enabled)
    {
    	// Change current directory to a temporary one
#ifdef _WIN32
    	if (destdir == 0) {
    		std::string destdirstr = gettmpdir();
    		!::SetCurrentDirectory(destdirstr.c_str());
    	}
    	else
    		!::SetCurrentDirectory(destdir);
        TCHAR szDirectory[MAX_PATH] = "";

        !::GetCurrentDirectory(sizeof(szDirectory) - 1, szDirectory);
        fprintf(stderr,"CWD %s\n",szDirectory);
#else
        if (destdir == 0)
        	chdir(gettmpdir().c_str());
        else
        	chdir(destdir);
#endif
    }
      
    if (bindaddr!=Address()) { // seeding
        if (Listen(bindaddr)<=0)
            quit("cant listen to %s\n",bindaddr.str())
    } else if (tracker!=Address() || httpgw_enabled || cmdgw_enabled) { // leeching
    	evutil_socket_t sock = INVALID_SOCKET;
        for (int i=0; i<=10; i++) {
            bindaddr = Address((uint32_t)INADDR_ANY,0);
            sock = Listen(bindaddr);
            if (sock>0)
                break;
            if (i==10)
                quit("cant listen on %s\n",bindaddr.str());
        }
        fprintf(stderr,"swift: My listen port is %d\n", BoundAddress(sock).port() );
    }

    if (tracker!=Address())
        SetTracker(tracker);

    if (httpgw_enabled)
        InstallHTTPGateway(Channel::evbase,httpaddr,chunk_size,maxspeed);
    if (cmdgw_enabled)
		InstallCmdGateway(Channel::evbase,cmdaddr,httpaddr);

    // TRIALM36: Allow browser to retrieve stats via AJAX and as HTML page
    if (statsaddr != Address())
    	InstallStatsGateway(Channel::evbase,statsaddr);

    if (root_hash!=Sha1Hash::ZERO && !filename)
        filename = strdup(root_hash.hex().c_str());

    if (!livesource_input && filename) {

    	// Client mode: regular or live download
    	if (!livestream)
    		file = Open(filename,root_hash,Address(),false,chunk_size);
    	else
    		file = LiveOpen(filename,root_hash,Address(),false,chunk_size);

        if (file<=0)
            quit("cannot open file %s",filename);
        printf("Root hash: %s\n", RootMerkleHash(file).hex().c_str());

        // RATELIMIT
        ContentTransfer *ct = ContentTransfer::transfer(file);
        ct->SetMaxSpeed(DDIR_DOWNLOAD,maxspeed[DDIR_DOWNLOAD]);
        ct->SetMaxSpeed(DDIR_UPLOAD,maxspeed[DDIR_UPLOAD]);
    }
    else if (livesource_input)
	{
		// LIVE
    	// Server mode: read from http source or pipe or file
		livesource_evb = evbuffer_new();

		if (strncmp(livesource_input,"http:",strlen("http:")))
		{
			// Source is file or pipe
			if (!strcmp(livesource_input,"-"))
				livesource_fd = 0; // stdin, aka read from pipe
			else {
				livesource_fd = open(livesource_input,READFLAGS,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
				if (livesource_fd < 0)
					quit("Could not open source input");
			}

			livesource_lt = swift::Create(filename);

			evtimer_assign(&evlivesource, Channel::evbase, LiveSourceFileTimerCallback, NULL);
			evtimer_add(&evlivesource, tint2tv(TINT_SEC));
		}
		else
		{
			// Source is HTTP server
		    char *token = NULL, *savetok = NULL;
			char *url = livesource_input;
			bool hasport = (bool)(strchr(&livesource_input[strlen("http:")],':') != NULL);

			// Parse URL
		    char *httpservstr=NULL,*httpportstr,*pathstr=NULL;

		    token = strtok_r(url,"/",&savetok); // tswift://
			if (token == NULL)
		        quit("Live source URL incorrect, no //");
			if (hasport)
			{
				token = strtok_r(NULL,":",&savetok);
				if (token == NULL)
					quit("Live source URL incorrect, no port");
				httpservstr = token+1;
				token = strtok_r(NULL,"/",&savetok);
				if (token == NULL)
					quit("Live source URL incorrect, no path");
				httpportstr = token;
				token = strtok_r(NULL,"",&savetok);
				if (token == NULL)
					quit("Live source URL incorrect, no end");
				pathstr = token;
			}
			else
			{
				token = strtok_r(NULL,"/",&savetok);
				if (token == NULL)
					quit("Live source URL incorrect, no path2");
				httpservstr = token;
				httpportstr = "80";
				token = strtok_r(NULL,"",&savetok);
				if (token == NULL)
					quit("Live source URL incorrect, no end2");
				pathstr = token;
			}

			int port=-1;
			int n = sscanf(httpportstr,"%u",&port);
			if (n != 1)
				quit("Live source URL incorrect, port no number");
			char *slashpath = (char *)malloc(strlen(pathstr)+1+1);
			strcpy(slashpath,"/");
			strcat(slashpath,pathstr);

			fprintf(stderr,"live: http: Reading from serv %s port %d path %s\n", httpservstr, port, slashpath );

			livesource_lt = swift::Create(filename);

			struct evhttp_connection *cn = evhttp_connection_base_new(Channel::evbase, NULL, httpservstr, port);
			struct evhttp_request *req = evhttp_request_new(LiveSourceHTTPResponseCallback, NULL);
			evhttp_request_set_chunked_cb(req,LiveSourceHTTPDownloadChunkCallback);
			evhttp_make_request(cn, req, EVHTTP_REQ_GET,slashpath);
			evhttp_add_header(req->output_headers, "Host", httpservstr);
		}
	}
    else if (!cmdgw_enabled && !httpgw_enabled)
    	quit("Not client, not live server, not a gateway?");

    // End after wait_time
    if (wait_time != TINT_NEVER && (long)wait_time > 0) {
    	evtimer_assign(&evend, Channel::evbase, EndCallback, NULL);
    	evtimer_add(&evend, tint2tv(wait_time));
    }

    // Arno: always, for statsgw, rate control, etc.
	evtimer_assign(&evreport, Channel::evbase, ReportCallback, NULL);
	evtimer_add(&evreport, tint2tv(TINT_SEC));

	fprintf(stderr,"swift: Mainloop\n");
	// Enter libevent mainloop
    event_base_dispatch(Channel::evbase);

    // event_base_loopexit() was called, shutting down
    if (file!=-1)
        Close(file);

    if (Channel::debug_file)
        fclose(Channel::debug_file);

    swift::Shutdown();

    return 0;
}


void ReportCallback(int fd, short event, void *arg) {

	if (file >= 0)
	{
		if (report_progress) {
			fprintf(stderr,
				"%s %lli of %lli (seq %lli) %lli dgram %lli bytes up, "	\
				"%lli dgram %lli bytes down\n",
				IsComplete(file) ? "DONE" : "done",
				Complete(file), Size(file), SeqComplete(file),
				Channel::global_dgrams_up, Channel::global_raw_bytes_up,
				Channel::global_dgrams_down, Channel::global_raw_bytes_down );
		}

        ContentTransfer *ct = ContentTransfer::transfer(file);
        if (report_progress) { // TODO: move up
        	fprintf(stderr,"upload %lf\n",ct->GetCurrentSpeed(DDIR_UPLOAD));
        	fprintf(stderr,"dwload %lf\n",ct->GetCurrentSpeed(DDIR_DOWNLOAD));
        }
        // Update speed measurements such that they decrease when DL/UL stops
        // Always
    	ct->OnRecvData(0);
    	ct->OnSendData(0);

    	// CHECKPOINT
    	if (ct->ttype() == FILE_TRANSFER && file_enable_checkpoint && !file_checkpointed && IsComplete(file))
    	{
    		HashTree *ht = ((FileTransfer *)ct)->hashtree();
    		std::string binmap_filename = ht->filename();
    		binmap_filename.append(".mbinmap");
    		fprintf(stderr,"swift: Complete, checkpointing %s\n", binmap_filename.c_str() );
    		FILE *fp = fopen(binmap_filename.c_str(),"wb");
    		if (!fp) {
    			print_error("cannot open mbinmap for writing");
    			return;
    		}
    		if (ht->serialize(fp) < 0)
    			print_error("writing to mbinmap");
    		else
    			file_checkpointed = true;
    		fclose(fp);
    	}
	}
    if (httpgw_enabled)
    {
        fprintf(stderr,".");

        // ARNOSMPTODO: Restore fail behaviour when used in SwarmPlayer 3000.
        if (!HTTPIsSending()) {
        	// TODO
        	//event_base_loopexit(Channel::evbase, NULL);
            return;
        }
    }
    if (StatsQuit())
    {
    	// SwarmPlayer 3000: User click "Quit" button in webUI.
    	struct timeval tv;
    	tv.tv_sec = 1;
    	int ret = event_base_loopexit(Channel::evbase,&tv);
    }
	// SWIFTPROC
	// ARNOSMPTODO: SCALE: perhaps less than once a second if many swarms
	CmdGwUpdateDLStatesCallback();

	// Gertjan fix
	// Arno, 2011-10-04: Temp disable
    //if (do_nat_test)
    //     nat_test_update();

	evtimer_add(&evreport, tint2tv(TINT_SEC));
}

void EndCallback(int fd, short event, void *arg) {
    event_base_loopexit(Channel::evbase, NULL);
}


void LiveSourceFileTimerCallback(int fd, short event, void *arg) {

	char buf[102400];

	fprintf(stderr,"live: file: timer\n");

	int nread = read(livesource_fd,buf,sizeof(buf));

	fprintf(stderr,"live: file: read returned %d\n", nread );

	if (nread < -1)
		print_error("error reading from live source");
	else if (nread > 0)
	{
		int ret = evbuffer_add(livesource_evb,buf,nread);
		if (ret < 0)
			print_error("live: file: error evbuffer_add");

		LiveSourceAttemptCreate();
	}

	// Reschedule
	evtimer_assign(&evlivesource, Channel::evbase, LiveSourceFileTimerCallback, NULL);
	evtimer_add(&evlivesource, tint2tv(TINT_SEC/10));
}


void LiveSourceHTTPResponseCallback(struct evhttp_request *req, void *arg)
{
	const char *new_location = NULL;
	switch(req->response_code)
	{
		case HTTP_OK:
			fprintf(stderr,"live: http: GET OK\n");
			break;

		case HTTP_MOVEPERM:
		case HTTP_MOVETEMP:
			new_location = evhttp_find_header(req->input_headers, "Location");
			fprintf(stderr,"live: http: GET REDIRECT %s\n", new_location );
			break;
		default:
			fprintf(stderr,"live: http: GET ERROR %d\n", req->response_code );
			event_base_loopexit(Channel::evbase, 0);
			return;
	}

	// LIVETODO: already reply data here?
	//evbuffer_add_buffer(ctx->buffer, req->input_buffer);
}


void LiveSourceHTTPDownloadChunkCallback(struct evhttp_request *req, void *arg)
{
	int length = evbuffer_get_length(req->input_buffer);
	fprintf(stderr,"live: http: read %d bytes\n", length );

	// Create chunks of chunk_size()
	int ret = evbuffer_add_buffer(livesource_evb,req->input_buffer);
	if (ret < 0)
		print_error("live: http: error evbuffer_add");

	LiveSourceAttemptCreate();
}


void LiveSourceAttemptCreate()
{
	if (evbuffer_get_length(livesource_evb) > livesource_lt->chunk_size())
	{
		size_t nchunklen = livesource_lt->chunk_size() * (size_t)(evbuffer_get_length(livesource_evb)/livesource_lt->chunk_size());
		uint8_t *chunks = evbuffer_pullup(livesource_evb, nchunklen);
		int nwrite = swift::Write(livesource_lt, chunks, nchunklen, -1);
		if (nwrite < -1)
			print_error("error creating live chunk");

		int ret = evbuffer_drain(livesource_evb, nchunklen);
		if (ret < 0)
			print_error("live: http: error evbuffer_drain");
	}
}




#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    return main(__argc,__argv);
}
#endif

