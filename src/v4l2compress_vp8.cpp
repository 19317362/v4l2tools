/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** v4l2compress_vp8.cpp
** 
** Read YUYV from a V4L2 capture -> compress in VP8 -> write to a V4L2 output device
** 
** -------------------------------------------------------------------------*/

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <fstream>

#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"

#include "libyuv.h"

#include "logger.h"

#include "V4l2Device.h"
#include "V4l2Capture.h"
#include "V4l2Output.h"


int stop=0;

/* ---------------------------------------------------------------------------
**  SIGINT handler
** -------------------------------------------------------------------------*/
void sighandler(int)
{ 
       printf("SIGINT\n");
       stop =1;
}

/* ---------------------------------------------------------------------------
**  main
** -------------------------------------------------------------------------*/
int main(int argc, char* argv[]) 
{	
	int verbose=0;
	const char *in_devname = "/dev/video0";	
	const char *out_devname = "/dev/video1";	
	int width = 640;
	int height = 480;	
	int fps = 25;	
	int c = 0;
	V4l2DeviceFactory::IoType ioTypeIn  = V4l2DeviceFactory::IOTYPE_MMAP;
	V4l2DeviceFactory::IoType ioTypeOut = V4l2DeviceFactory::IOTYPE_MMAP;
	int bitrate = 1000;
	vpx_rc_mode ratecontrolmode = VPX_VBR;
	
	while ((c = getopt (argc, argv, "hW:H:P:F:v::rw" "cb:")) != -1)
	{
		switch (c)
		{
			case 'v':	verbose = 1; if (optarg && *optarg=='v') verbose++;  break;
			case 'W':	width = atoi(optarg); break;
			case 'H':	height = atoi(optarg); break;
			case 'F':	fps = atoi(optarg); break;
			case 'b':	bitrate = atoi(optarg); break;
			case 'c':	ratecontrolmode = VPX_CBR; break;
			case 'r':	ioTypeIn  = V4l2DeviceFactory::IOTYPE_READ; break;			
			case 'w':	ioTypeOut = V4l2DeviceFactory::IOTYPE_READ; break;	
			case 'h':
			{
				std::cout << argv[0] << " [-v[v]] [-W width] [-H height] source_device dest_device" << std::endl;
				std::cout << "\t -v            : verbose " << std::endl;
				std::cout << "\t -vv           : very verbose " << std::endl;

				std::cout << "\t -b bitrate           : target bitrate " << std::endl;
				std::cout << "\t -c                   : rate control mode CBR (default is VBR) " << std::endl;

				std::cout << "\t -W width             : V4L2 capture width (default "<< width << ")" << std::endl;
				std::cout << "\t -H height            : V4L2 capture height (default "<< height << ")" << std::endl;
				std::cout << "\t -F fps               : V4L2 capture framerate (default "<< fps << ")" << std::endl;
				std::cout << "\t -r                   : V4L2 capture using read interface (default use memory mapped buffers)" << std::endl;
				std::cout << "\t -w                   : V4L2 capture using write interface (default use memory mapped buffers)" << std::endl;
				std::cout << "\t source_device        : V4L2 capture device (default "<< in_devname << ")" << std::endl;
				std::cout << "\t dest_device          : V4L2 capture device (default "<< out_devname << ")" << std::endl;
				exit(0);
			}
		}
	}
	if (optind<argc)
	{
		in_devname = argv[optind];
		optind++;
	}	
	if (optind<argc)
	{
		out_devname = argv[optind];
		optind++;
	}	


	// initialize log4cpp
	initLogger(verbose);

	vpx_image_t          raw;
	if(!vpx_img_alloc(&raw, VPX_IMG_FMT_I420, width, height, 1))
	{
		LOG(WARN) << "vpx_img_alloc"; 
	}

	const vpx_codec_iface_t* algo = vpx_codec_vp8_cx();
	vpx_codec_enc_cfg_t  cfg;
	if (vpx_codec_enc_config_default(algo, &cfg, 0) != VPX_CODEC_OK)
	{
		LOG(WARN) << "vpx_codec_enc_config_default"; 
	}

	cfg.g_w = width;
	cfg.g_h = height;	
	cfg.rc_end_usage = ratecontrolmode;
	cfg.rc_target_bitrate = bitrate;
	
	vpx_codec_ctx_t      codec;
	if(vpx_codec_enc_init(&codec, algo, &cfg, 0))    
	{
		LOG(WARN) << "vpx_codec_enc_init"; 
	}
		
	// init V4L2 capture interface
	int format = V4L2_PIX_FMT_YUYV;
	V4L2DeviceParameters param(in_devname,format,width,height,fps,verbose);
	V4l2Capture* videoCapture = V4l2DeviceFactory::CreateVideoCapture(param, ioTypeIn);
	
	if (videoCapture == NULL)
	{	
		LOG(WARN) << "Cannot create V4L2 capture interface for device:" << in_devname; 
	}
	else
	{
		// init V4L2 output interface
		V4L2DeviceParameters outparam(out_devname, V4L2_PIX_FMT_VP8, videoCapture->getWidth(), videoCapture->getHeight(), 0, verbose);
		V4l2Output* videoOutput = V4l2DeviceFactory::CreateVideoOutput(outparam, ioTypeOut);
		if (videoOutput == NULL)
		{	
			LOG(WARN) << "Cannot create V4L2 output interface for device:" << out_devname; 
		}
		else
		{		
			LOG(NOTICE) << "Start Capturing from " << in_devname; 
			timeval tv;
			int flags=0;
			int frame_cnt=0;
			
			signal(SIGINT,sighandler);
			LOG(NOTICE) << "Start Compressing " << in_devname << " to " << out_devname; 
			while (!stop) 
			{
				tv.tv_sec=1;
				tv.tv_usec=0;
				int ret = videoCapture->isReadable(&tv);
				if (ret == 1)
				{
					char buffer[videoCapture->getBufferSize()];
					int rsize = videoCapture->read(buffer, sizeof(buffer));
					
					ConvertToI420((const uint8*)buffer, rsize,
						raw.planes[0], width,
						raw.planes[1], width/2,
						raw.planes[2], width/2,
						0, 0,
						width, height,
						width, height,
						libyuv::kRotate0, libyuv::FOURCC_YUY2);
													
					if(vpx_codec_encode(&codec, &raw, frame_cnt++, 1, flags, VPX_DL_REALTIME))    
					{					
						LOG(WARN) << "vpx_codec_encode: " << vpx_codec_error(&codec) << "(" << vpx_codec_error_detail(&codec) << ")";
					}
					
					vpx_codec_iter_t iter = NULL;
					const vpx_codec_cx_pkt_t *pkt;
					while( (pkt = vpx_codec_get_cx_data(&codec, &iter)) ) 
					{
						if (pkt->kind==VPX_CODEC_CX_FRAME_PKT)
						{
							int wsize = videoOutput->write((char*)pkt->data.frame.buf, pkt->data.frame.sz);
							LOG(DEBUG) << "Copied " << rsize << " " << wsize; 
						}
						else
						{
							break;
						}
					}
				}
				else if (ret == -1)
				{
					LOG(NOTICE) << "stop " << strerror(errno); 
					stop=1;
				}
			}
			delete videoOutput;
		}
		delete videoCapture;
	}
	
	return 0;
}
