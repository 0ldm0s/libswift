/*
 *  livetreetest.cpp
 *
 *  Created by Arno Bakker
 *  Copyright 2009-2016 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 *
 */
#include "livetree.h"
#include "bin_utils.h"
#include <gtest/gtest.h>


using namespace swift;

typedef std::vector<char *>	    clist_t;
typedef std::map<bin_t,Sha1Hash>  hmap_t;




void do_add_data(LiveHashTree *umt, int nchunks)
{
    for (int i=0; i<nchunks; i++)
    {
	char data[1024];
	memset(data,i%255,1024);

	umt->AddData(data,1024);
	umt->sane_tree();
    }
}

#ifdef LATER
TEST(LiveTreeTest,AddData10)
{
    LiveHashTree *umt = new LiveHashTree(481);
    do_add_data(umt,10);

    // asserts
    ASSERT_EQ(umt->peak_count(), 2);
    ASSERT_EQ(umt->peak(0), bin_t(3,0));
    ASSERT_EQ(umt->peak(1), bin_t(1,4));

}
#endif

void do_download(LiveHashTree *umt, int nchunks, hmap_t &hmap)
{
    bin_t peak_bins_[64];
    int peak_count = gen_peaks(nchunks,peak_bins_);
    fprintf(stderr,"peak count %d\n", peak_count );

    // Send peaks
    fprintf(stderr,"Sending Peaks\n");
    for (int i=0; i<peak_count; i++)
    {
	//umt->AddData(data,1024);
	Sha1Hash peakhash = hmap[peak_bins_[i]];
	ASSERT_TRUE(umt->OfferSignedPeakHash(peak_bins_[i],peakhash.bits));
	umt->sane_tree();
    }

    for (int i=0; i<nchunks; i++)
    {
	//int r=nchunks-1-i;
	int r=i;
	fprintf(stderr,"\nAdd %u\n", r);

	bin_t orig(0,r);
	bin_t pos = orig;
	bin_t peak = umt->peak_for(pos);
	fprintf(stderr,"Add: %u peak %s\n", r, peak.str().c_str() );

	// Sending uncles
	binvector bv;
	while (pos!=peak)
	{
	    bin_t uncle = pos.sibling();
	    bv.push_back(uncle);
	    pos = pos.parent();
	}
	binvector::reverse_iterator iter;
	for (iter=bv.rbegin(); iter != bv.rend(); iter++)
	{
	    bin_t uncle = *iter;
	    fprintf(stderr,"Add %u uncle %s\n", r, uncle.str().c_str() );
	    umt->OfferHash(uncle,hmap[uncle]);
	    umt->sane_tree();
	}

	// Sending actual
	fprintf(stderr,"Add %u orig %s\n", r, orig.str().c_str() );
	ASSERT_TRUE(umt->OfferHash(orig,hmap[orig]));
	umt->sane_tree();
    }
}


LiveHashTree *prepare_do_download(int nchunks)
{
    fprintf(stderr,"\nprepare_do_download(%d)\n", nchunks);

    // Create chunks
    clist_t	clist;
    for (int i=0; i<nchunks; i++)
    {
	char *data = new char[1024];
	memset(data,i%255,1024);
	clist.push_back(data);
    }

    // Create leaves
    hmap_t hmap;
    for (int i=0; i<nchunks; i++)
    {
	hmap[bin_t(0,i)] = Sha1Hash(clist[i],1024);
	fprintf(stderr,"Hash leaf %s\n", bin_t(0,i).str().c_str() );

    }

    // Pad with zero hashes
    int height = ceil(log2((double)nchunks));

    fprintf(stderr,"Hash height %d\n", height );

    int width = pow(2.0,height);
    for (int i=nchunks; i<width; i++)
    {
	hmap[bin_t(0,i)] = Sha1Hash::ZERO;
	fprintf(stderr,"Hash empty %s\n", bin_t(0,i).str().c_str() );
    }

    // Calc hashtree
    for (int h=1; h<height+1; h++)
    {
	int step = pow(2.0,h);
	int npairs = width/step;
	for (int i=0; i<npairs; i++)
	{
	    bin_t b(h,i);
	    Sha1Hash parhash(hmap[b.left()],hmap[b.right()]);
	    hmap[b] = parhash;

	    fprintf(stderr,"Hash parent %s\n", b.str().c_str() );
	}
    }

    LiveHashTree *umt = new LiveHashTree(481,true); // pubkey
    do_download(umt,nchunks,hmap);

    for (int i=0; i<nchunks; i++)
    {
	delete clist[i];
    }

    return umt;
}


TEST(LiveTreeTest,Download8)
{
    LiveHashTree *umt = prepare_do_download(8);

    // asserts
    ASSERT_EQ(umt->peak_count(), 1);
    ASSERT_EQ(umt->peak(0), bin_t(3,0));
}


TEST(LiveTreeTest,Download10)
{
    LiveHashTree *umt = prepare_do_download(10);

    // asserts
    ASSERT_EQ(umt->peak_count(), 2);
    ASSERT_EQ(umt->peak(0), bin_t(3,0));
    ASSERT_EQ(umt->peak(1), bin_t(1,4));

}

TEST(LiveTreeTest,Download11)
{
    LiveHashTree *umt = prepare_do_download(11);

    // asserts
    ASSERT_EQ(umt->peak_count(), 3);
    ASSERT_EQ(umt->peak(0), bin_t(3,0));
    ASSERT_EQ(umt->peak(1), bin_t(1,4));
    ASSERT_EQ(umt->peak(2), bin_t(0,10));
}


TEST(LiveTreeTest,DownloadIter)
{
    for (int i=0; i<17; i++)
    {
	LiveHashTree *umt = prepare_do_download(i);
    }
}


int main (int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
