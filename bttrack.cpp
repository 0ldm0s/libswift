/*
 *  bttrack.cpp
 *
 *  Implements a BitTorrent tracker client. If SwarmIDs are not SHA1 hashes
 *  they are hashed with a SHA1 MDC to turn them into an infohash.
 *
 *  Only HTTP trackers supported at the moment.
 *
 *
 *  DEALLOC RETURN VALUES OF ParseBencoded
 *
 *  DEALLOC uriencode return values.
 *
 *  Created by Arno Bakker
 *  Copyright 2013-2016 Vrije Universiteit Amsterdam. All rights reserved.
 */
#include "swift.h"
#include <event2/http.h>
#include <event2/http_struct.h>
#include <sstream>


using namespace swift;


// https://wiki.theory.org/BitTorrentSpecification#peer_id
#define BT_PEER_ID_LENGTH	20 // bytes
#define BT_PEER_ID_PREFIX	"-SW1000-"

#define BT_EVENT_STARTED	"started"
#define BT_EVENT_COMPLETED	"completed"
#define BT_EVENT_STOPPED	"stopped"
#define BT_EVENT_WORKING	""

#define BT_BENCODE_STRING_SEP		":"
#define BT_BENCODE_INT_SEP		"e"

#define BT_FAILURE_REASON_DICT_KEY	"failure reason"
#define BT_PEERS_IPv4_DICT_KEY	"peers"
#define BT_INTERVAL_DICT_KEY	"interval"
#define BT_PEERS_IPv6_DICT_KEY	"peers6"

typedef enum
{
    BENCODED_INT,
    BENCODED_STRING
} bencoded_type_t;


static int ParseBencodedPeers(struct evbuffer *evb, std::string key, peeraddrs_t *peerlist);
static int ParseBencodedValue(struct evbuffer *evb, struct evbuffer_ptr &startevbp, std::string key, bencoded_type_t valuetype, char **valueptr);

BTTrackerClient::BTTrackerClient(std::string url) : url_(url)
{
    // Create PeerID
    peerid_ = new uint8_t[BT_PEER_ID_LENGTH];

    int ret = 0;
#ifdef OPENSSL
    strcpy((char *)peerid_,BT_PEER_ID_PREFIX);
    ret = RAND_bytes(&peerid_[strlen(BT_PEER_ID_PREFIX)], BT_PEER_ID_LENGTH-strlen(BT_PEER_ID_PREFIX));
#endif
    if (ret != 1)
    {
	// Fallback and no OPENSSL option
	char buf[32];
	std::ostringstream oss;
	oss << BT_PEER_ID_PREFIX;
	sprintf(buf,"%05d", rand() );
	oss << buf;
	sprintf(buf,"%05d", rand() );
	oss << buf;
	sprintf(buf,"%05d", rand() );
	oss << buf;
	std::string randstr = oss.str().substr(0,BT_PEER_ID_LENGTH);
	strcpy((char *)peerid_,randstr.c_str());
    }
}

BTTrackerClient::~BTTrackerClient()
{
    if (peerid_ != NULL)
	delete peerid_;
}


int BTTrackerClient::Contact(ContentTransfer &transfer, std::string event, bttrack_peerlist_callback_t callback)
{
    Address myaddr = Channel::BoundAddress(Channel::default_socket());
    std::string q = CreateQuery(transfer,myaddr,event);
    if (q.length() == 0)
	return -1;
    else
	return HTTPConnect(q,callback);
}

/** IP in myaddr currently unused */
std::string BTTrackerClient::CreateQuery(ContentTransfer &transfer, Address myaddr, std::string event)
{
    Sha1Hash infohash;

    // Should be per swarm, now using global upload, just to monitor sharing activity
    uint64_t uploaded = Channel::global_bytes_up;
    uint64_t downloaded = swift::SeqComplete(transfer.td());
    uint64_t left = 0;
    if (transfer.ttype() == FILE_TRANSFER)
    {
	infohash = transfer.swarm_id().roothash();
	if (downloaded > swift::Size(transfer.td()))
	    left = 0;
	else
	    left = swift::Size(transfer.td()) - downloaded;
    }
    else
    {
	SwarmPubKey spubkey = transfer.swarm_id().spubkey();
	infohash = Sha1Hash(spubkey.bits(),spubkey.length());
	left = 0x7fffffffffffffff;
    }

    // See
    // http://www.bittorrent.org/beps/bep_0003.html
    // https://wiki.theory.org/BitTorrent_Tracker_Protocol

    char *esc = NULL;
    std::ostringstream oss;

    oss << "info_hash=";
    esc = evhttp_uriencode((const char *)infohash.bytes(),Sha1Hash::SIZE,false);
    if (esc == NULL)
	return "";
    oss << esc;
    free(esc);
    oss << "&";

    oss << "peer_id=";
    esc = evhttp_uriencode((const char *)peerid_,BT_PEER_ID_LENGTH,false);
    if (esc == NULL)
	return "";
    oss << esc;
    free(esc);
    oss << "&";

    // ip= currently unused

    oss << "port=";
    oss << myaddr.port();
    oss << "&";

    oss << "uploaded=";
    oss << uploaded;
    oss << "&";

    oss << "downloaded=";
    oss << downloaded;
    oss << "&";

    oss << "left=";
    oss << left;
    oss << "&";

    // Request compacted peerlist, is most common http://www.bittorrent.org/beps/bep_0023.html
    oss << "compact=1";

    if (event.length() > 0)
    {
	oss << "&";
	oss << "event=";
	oss << event;
    }

    return oss.str();
}


static void BTTrackerClientHTTPResponseCallback(struct evhttp_request *req, void *callbackvoid)
{
    bttrack_peerlist_callback_t callback = (bttrack_peerlist_callback_t)callbackvoid;

    fprintf(stderr,"bttrack: Callback: ENTER\n" );

    if (req->response_code != HTTP_OK)
    {
	if (callback != NULL)
	    callback("Invalid HTTP Response Code",0,peeraddrs_t());
	return;
    }

    // Create a copy of the response, as we do destructive parsing
    struct evbuffer *evb = evhttp_request_get_input_buffer(req);
    size_t copybuflen = evbuffer_get_length(evb);
    char *copybuf = new char[copybuflen];
    int ret = evbuffer_remove(evb,copybuf,copybuflen);
    if (ret < 0)
    {
	delete copybuf;

	if (callback != NULL)
	    callback("Invalid HTTP Response Code",0,peeraddrs_t());
	return;
    }

    evb = evbuffer_new();
    evbuffer_add(evb,copybuf,copybuflen);

    // Find "failure reason" string following BT spec
    struct evbuffer_ptr startevbp = evbuffer_search(evb,BT_FAILURE_REASON_DICT_KEY,strlen(BT_FAILURE_REASON_DICT_KEY),NULL);
    if (startevbp.pos != -1)
    {
	char *valuebytes = NULL;
	std::string errorstr;
	int ret = ParseBencodedValue(evb,startevbp,BT_FAILURE_REASON_DICT_KEY,BENCODED_STRING,&valuebytes);
	if (ret < 0)
	{
	    errorstr = "Error parsing tracker response: failure reason";
	}
	else
	{
	    errorstr = "Tracker responded: "+std::string(valuebytes);
	}
	if (callback != NULL)
	    callback(errorstr,0,peeraddrs_t());

	evbuffer_free(evb);
	delete copybuf;
	return; // failure case done
    }
    evbuffer_free(evb);

    // If not failure, find tracker report interval
    uint32_t interval=0;

    evb = evbuffer_new();
    evbuffer_add(evb,copybuf,copybuflen);
    startevbp = evbuffer_search(evb,BT_INTERVAL_DICT_KEY,strlen(BT_INTERVAL_DICT_KEY),NULL);
    if (startevbp.pos != -1)
    {
	char *valuebytes = NULL;
	std::string errorstr="";
	int ret = ParseBencodedValue(evb,startevbp,BT_INTERVAL_DICT_KEY,BENCODED_INT,&valuebytes);
	if (ret < 0)
	{
	    delete copybuf;
	    evbuffer_free(evb);

	    if (callback != NULL)
		callback("Error parsing tracker response: interval",0,peeraddrs_t());

	    return;
	}
	else
	{
	    fprintf(stderr,"btrack: Interval string parsed %s\n", valuebytes );

	    ret = sscanf(valuebytes,"%u",&interval);
	    if (ret != 1)
	    {
		delete copybuf;
		evbuffer_free(evb);

		if (callback != NULL)
		    callback("Error parsing tracker response: interval",0,peeraddrs_t());

		return;
	    }

	    fprintf(stderr,"btrack: Got interval %u\n", interval);
	}
    }
    evbuffer_free(evb);



    // If not failure, find peers key whose value is compact IPv4 addresses
    // http://www.bittorrent.org/beps/bep_0023.html
    peeraddrs_t	peerlist;

    evb = evbuffer_new();
    evbuffer_add(evb,copybuf,copybuflen);
    ret = ParseBencodedPeers(evb, BT_PEERS_IPv4_DICT_KEY,&peerlist);
    if (ret < 0)
    {
	delete copybuf;
	evbuffer_free(evb);

	if (callback != NULL)
	    callback("Error parsing tracker response: peerlist",interval,peeraddrs_t());
	return;
    }
    evbuffer_free(evb);

    // If not failure, find peers key whose value is compact IPv6 addresses
    // http://www.bittorrent.org/beps/bep_0007.html

    delete copybuf;
}



int BTTrackerClient::HTTPConnect(std::string query,bttrack_peerlist_callback_t callback)
{
    std::string fullurl = url_+"?"+query;

    fprintf(stderr,"bttrack: HTTPConnect: %s\n", fullurl.c_str() );

    struct evhttp_uri *evu = evhttp_uri_parse(fullurl.c_str());
    if (evu == NULL)
	return -1;

    int fullpathlen = strlen(evhttp_uri_get_path(evu))+strlen("?")+strlen(evhttp_uri_get_query(evu));
    char *fullpath = new char[fullpathlen+1];
    strcpy(fullpath,evhttp_uri_get_path(evu));
    strcat(fullpath,"?");
    strcat(fullpath,evhttp_uri_get_query(evu));

    fprintf(stderr,"bttrack: HTTPConnect: Composed fullpath %s\n", fullpath );

    // Create HTTP client
    struct evhttp_connection *cn = evhttp_connection_base_new(Channel::evbase, NULL, evhttp_uri_get_host(evu), evhttp_uri_get_port(evu) );
    struct evhttp_request *req = evhttp_request_new(BTTrackerClientHTTPResponseCallback, (void *)callback);

    // Make request to server
    fprintf(stderr,"bttrack: HTTPConnect: Making request\n" );

    evhttp_make_request(cn, req, EVHTTP_REQ_GET, fullpath);
    evhttp_add_header(req->output_headers, "Host", evhttp_uri_get_host(evu));

    delete fullpath;

    fprintf(stderr,"bttrack: HTTPConnect: Exit\n" );

    return 0;
}


static int ParseBencodedPeers(struct evbuffer *evb, std::string key, peeraddrs_t *peerlist)
{
    struct evbuffer_ptr startevbp = evbuffer_search(evb,key.c_str(),key.length(),NULL);
    if (startevbp.pos != -1)
    {
	char *valuebytes = NULL;
	std::string errorstr;
	int ret = ParseBencodedValue(evb,startevbp,key,BENCODED_STRING,&valuebytes);
	if (ret < 0)
	    return -1;

	int peerlistenclen = ret;
	fprintf(stderr,"bttrack: Peerlist encoded len %d\n", peerlistenclen );

	// Decompact addresses
	struct evbuffer *evb2 = evbuffer_new();
	evbuffer_add(evb2,valuebytes,peerlistenclen);


	int family=AF_INET;
	int enclen=6;
	if (key == BT_PEERS_IPv6_DICT_KEY)
	{
	    family = AF_INET6;
	    enclen = 18;
	}
	for (int i=0; i<peerlistenclen/enclen; i++)
	{
	    // Careful: if PPSPP on the wire encoding changes, we can't use
	    // this one anymore.
	    Address addr = evbuffer_remove_pexaddr(evb2,family);
	    peerlist->push_back(addr);

	    fprintf(stderr,"bttrack: Peerlist parsed %d %s\n", i, addr.str().c_str() );
	}
	evbuffer_free(evb2);

	return 0;
    }
    else
	return -1;
}


/** Failure reported, extract string from bencoded dictionary */
static int ParseBencodedValue(struct evbuffer *evb, struct evbuffer_ptr &startevbp, std::string key, bencoded_type_t valuetype, char **valueptr)
{
    fprintf(stderr,"bttrack: Callback: key %s starts at " PRISIZET "\n", key.c_str(), startevbp.pos );

    size_t pastkeypos = startevbp.pos+key.length();
    if (valuetype == BENCODED_INT)
	pastkeypos++; // skip 'i'

    fprintf(stderr,"bttrack: Callback: key ends at " PRISIZET "\n", pastkeypos );

    int ret = evbuffer_ptr_set(evb, &startevbp, pastkeypos, EVBUFFER_PTR_SET);
    if (ret < 0)
	return -1;

    char *separator = BT_BENCODE_STRING_SEP;
    if (valuetype == BENCODED_INT)
	separator = BT_BENCODE_INT_SEP;

    // Find separator to determine string len
    struct evbuffer_ptr endevbp = evbuffer_search(evb,separator,strlen(separator),&startevbp);
    if (endevbp.pos == -1)
	return -1;

    fprintf(stderr,"bttrack: Callback: separator at " PRISIZET " key len %d\n", endevbp.pos, key.length() );

    size_t intcstrlen = endevbp.pos - startevbp.pos;

    fprintf(stderr,"bttrack: Callback: value length " PRISIZET "\n", intcstrlen );

    size_t strpos = endevbp.pos+1;

    fprintf(stderr,"bttrack: Callback: value starts at " PRISIZET "\n", strpos );

    ret = evbuffer_ptr_set(evb, &startevbp, strpos, EVBUFFER_PTR_SET);
    if (ret < 0)
	return -1;

    // Remove all before
    ret = evbuffer_drain(evb,strpos-1-intcstrlen);
    if (ret < 0)
	return -1;

    char *intcstr = new char[intcstrlen+1];
    intcstr[intcstrlen] = '\0';
    ret = evbuffer_remove(evb,intcstr,intcstrlen);
    if (ret < 0)
    {
	delete intcstr;
	return -1;
    }


    fprintf(stderr,"bttrack: Callback: Length value string %s\n", intcstr );

    if (valuetype == BENCODED_INT)
    {
	*valueptr = intcstr;
	return intcstrlen;
    }
    // For strings carry on

    int slen=0;
    ret = sscanf(intcstr,"%d", &slen);
    delete intcstr;
    if (ret != 1)
	return -1;

    fprintf(stderr,"bttrack: Callback: Length value int %d\n", slen );




    // Drain colon
    ret = evbuffer_drain(evb,1);
    if (ret < 0)
	return -1;

    // NOTE: actual bytes may also contain '\0', C string is for convenience when it isn't pure binary.
    char *valuecstr = new char[slen+1];
    valuecstr[slen] = '\0';
    ret = evbuffer_remove(evb,valuecstr,slen);
    if (ret < 0)
    {
	delete valuecstr;
	return -1;
    }
    else
    {
	fprintf(stderr,"bttrack: Callback: Value string <%s>\n", valuecstr );

	*valueptr = valuecstr;
	// do not delete valuecstr;
	return slen;
    }
}


