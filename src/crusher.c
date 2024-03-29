
#include <string.h>
#include <assert.h>
#include <usb.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "crusher.h"
#include "crusher-usb.h"
#include "mg1264-io.h"
#include "mg1264.h"
#include "mg1264-mware.h"
#include "crusher-i2c.h"
#include "bswap.h"
#include "logging.h"

#define MAX_USB_ENCODERS 16

enum {
        CYPRRESS_NO_FW,
        CYPRRESS_HAVE_FW,
};

/* Device ID's: */
static struct {
	uint16_t idVendor;
	uint16_t idProduct;
	uint8_t  cyInit;
	uint8_t  devType;
} devices[] = {
	/* cypress without firmware */
	{0x06e1, 0xa160, CYPRRESS_NO_FW, DEV_TYPE_ENCODER},
	{0x0fd9, 0x0004, CYPRRESS_NO_FW, DEV_TYPE_ENCODER},
	/* this ID's shared between capture and HD-encoder devices that don't have firmware installed. */
    {0x04b4, 0x8613, CYPRRESS_NO_FW, DEV_TYPE_CAPTURE},
	/* cypress with firmware */
	{0x06e1, 0xa161, CYPRRESS_HAVE_FW, DEV_TYPE_ENCODER},
	{0x0fd9, 0x0005, CYPRRESS_HAVE_FW, DEV_TYPE_ENCODER},
	/* HACK: using turbo.264 fw for rdx-160 */
	{0x0fd9, 0xa161, CYPRRESS_HAVE_FW, DEV_TYPE_ENCODER},
	/* cypress with interlal eeprom firmware */
	{0x04b4, 0xbd1c, CYPRRESS_HAVE_FW, DEV_TYPE_CAPTURE},
	/* HD device */
    {0x0fd9, 0x0022, CYPRRESS_NO_FW, DEV_TYPE_ENCODER_HD},
    {0x0fd9, 0x0023, CYPRRESS_HAVE_FW, DEV_TYPE_ENCODER_HD},
	{0, 0, 0, 0}
};

/* default firmwares */
#ifdef DATADIR
static char FX2Firmware[] = DATADIR"/"FX2_Firmware;
static char MG1264Firmware[] = DATADIR"/"MG1264_Firmware;
#else
static char FX2Firmware[] = FX2_Firmware;
static char MG1264Firmware[] = MG1264_Firmware;
#endif

/*
 * checking presented device from DEVICE_LIST
 *  if founded device with firmware, returns 1
 *  if founded device without firmware, returns 2
 *  else returns 0
 */
static int8_t check_device(struct usb_device *dev)
{
    int i = 0;
    while(devices[i].idVendor != 0x0000 && devices[i].idProduct != 0x0000) {
		if(dev->descriptor.idVendor == devices[i].idVendor
				&& dev->descriptor.idProduct == devices[i].idProduct){
#if DBG > 1
				DEBUG("Device [%d] (0x%04x:0x%04x) - found.",
					i, devices[i].idVendor, devices[i].idProduct);
		    switch (devices[i].cyInit) {
		        case CYPRRESS_NO_FW:
		            DEBUG("Device (0x%04x:0x%04x)- FX2 without firmware.",
                        dev->descriptor.idVendor, dev->descriptor.idProduct);
		            break;
		        case CYPRRESS_HAVE_FW:
		            DEBUG("Device (0x%04x:0x%04x)- FX2 with firmware.",
		                dev->descriptor.idVendor, dev->descriptor.idProduct);
		            break;
		        default:
		            ERROR("Device (0x%04x:0x%04x)- have unknown state.",
		                                dev->descriptor.idVendor, dev->descriptor.idProduct);
		    }
#endif
		    return i;
		}
		i++;
    }
    return -1;
}


void crusher_defaults(crusher_t *crusher)
{
	int i;
	const char *dev_num = getenv ("CRUSHER_DEV");

	/* all to zero */
	memset ( crusher, 0x00, sizeof(crusher_t) );

	crusher->loglevel = 7;
	crusher->fx2fw = FX2Firmware;
	crusher->mg1264fw = MG1264Firmware;
	if(dev_num)
	    crusher->devnum = atoi(dev_num);

	crusher->framesToLoad = DEFAULT_LOAD_FRAMES;

	crusher->width = 320;
	crusher->height = 240;

	crusher->aspect_ratio_x = 1;
	crusher->aspect_ratio_y = 1;

	crusher->bitrate = 750000;
	crusher->bitrate_mode = BITRATE_MODE_AUTO;
	crusher->framerate_num = 90000;
	crusher->framerate_den = 3600;
	/* this values not for capture mode */
	crusher->rc_size = 2000000;
	crusher->scenecut_threshold = 1;
	crusher->deblock = 1;

	/* used only in capture */
	crusher->audio_codec = AUDIO_CODEC_AAC;
	crusher->audio_samplerate = 48000;
	crusher->audio_channels = 2;
	crusher->audio_bitrate = 128000;
	crusher->audio_gain = 0;
	crusher->video_input = 0;
	crusher->video_brightness = 0;
	crusher->video_contrast = 128;
	crusher->video_saturation_Cb = 128;
	crusher->video_saturation_Cr = 128;

	crusher->lastEvent = malloc(sizeof(event_t));
	crusher->event_complete = 1;

	crusher->out_format = OUT_FORMAT_VES;
	crusher->devtype = DEV_TYPE_AUTO;
	crusher->devmode = DEV_TYPE_AUTO;
	/* output framebuffer */
	for(i=0;i<6;i++)
		crusher->out_data[i].data = malloc(MAX_H264_FRAMESIZE);

	crusher->mware_data = calloc(MWARE_SIZE, sizeof(uint32_t));
}

usb_dev_handle	*crusher_usb_open(struct usb_device	*usbdevice)
{
	usb_dev_handle	*udev = usb_open(usbdevice);

	if (!udev) {
     	ERROR("Device Can't be opened.");
    	goto usb_open_fail;
	}

	if (usb_set_configuration(udev, 0x01) < 0){
		ERROR("configuration CAN'T be selected.");
    	goto usb_open_fail;
	}
	
	if (usb_claim_interface(udev, 0x00) < 0){
		ERROR("Interface NOT claimed.");
    	goto usb_open_fail;
	}
	
	if (usb_set_altinterface(udev, 0x00) < 0){
		ERROR("Interface NOT altied.");
    	goto usb_open_fail;
	}

	return udev;

usb_open_fail:
	if(udev)
		usb_close(udev);
	return NULL;
}

static int crusher_free(crusher_t *crusher)
{
	int i;
	event_t *event = (event_t *) crusher->lastEvent;
	block_config_chunk_t *blockConfig = (block_config_chunk_t *) crusher->blockConfig;

//	if(crusher->udev)
//		free(crusher->udev);

//	if(crusher->fx2fw)
//		free(crusher->fx2fw);
//	if(crusher->mg1264fw)
//		free(crusher->mg1264fw);

    if(crusher->mwarecfg)
        free(crusher->mwarecfg);
	if(crusher->mware_data)
		free(crusher->mware_data);
	if(crusher->textcfg)
		free(crusher->textcfg);

	if(crusher->input_buffer)
		free(crusher->input_buffer);

	if(crusher->lastEvent)
		free(event);

	if(crusher->blockConfig)
		free(blockConfig);

	for(i=0;i<6;i++)
		free(crusher->out_data[i].data);

	return 1;
}

void crusher_close(crusher_t *crusher)
{
	if (crusher->udev) {
//            usb_reset((usb_dev_handle  *) crusher->udev);
//            usb_release_interface((usb_dev_handle  *) crusher->udev, 0x00);
	    usb_reset((usb_dev_handle  *) crusher->udev);
        usb_close((usb_dev_handle	*) crusher->udev);
	}
	crusher_free(crusher);
}

/*
 * searching USB device with devnum, if presended, open it and return device_handle
 */
int crusher_find_device(crusher_t *crusher, int upload_fw)
{

    struct usb_bus		*bus = NULL;
	struct usb_device	*dev = NULL;
    uint8_t 		    devcounter;
	uint8_t 		    anti_loop = 0;
	int8_t				dev_type_id = -1;
	FILE			    *fx2Fw = NULL;

dev_search:
    usb_init();
    usb_find_busses();
    usb_find_devices();

    devcounter = 0;
	for (bus = usb_busses; bus; bus = bus->next) {

		for (dev = bus->devices; dev; dev = dev->next) {
			XTREME("CHECKING (0x%04x:0x%04x)...",
						dev->descriptor.idVendor, dev->descriptor.idProduct );
			dev_type_id = check_device(dev);
			if(dev_type_id<0) {
	             XTREME("Device (0x%04x:0x%04x)- Not encoder.",
	                     dev->descriptor.idVendor, dev->descriptor.idProduct);
	             continue;
			}

			switch (devices[dev_type_id].cyInit) {
                case CYPRRESS_HAVE_FW:
                    if(devcounter == crusher->devnum) {
                        crusher->devtype = devices[dev_type_id].devType;

                        if(crusher->devmode == DEV_TYPE_AUTO)
                            crusher->devmode = crusher->devtype;

                        switch(crusher->devtype) {
                            case DEV_TYPE_ENCODER:
                                crusher->mg1264fw = MG1264Firmware;
                                crusher->framerate_num = 2500;
                                crusher->framerate_den = 100;
                                break;
                            case DEV_TYPE_CAPTURE:
                                crusher->mg1264fw = MG1264_CaptureFirmware;
                                break;
                            case DEV_TYPE_ENCODER_HD:
                                crusher->mg1264fw = MG3500_HDFirmware;
                                break;
                            default:
                                ERROR("Strange device type %d", crusher->devtype);
                                return 0;
                        }
                        DEBUG("MG1264/MG3500 firmware: %s.", crusher->mg1264fw);
                        mware_defaults(crusher);

			            crusher->udev = (void *) crusher_usb_open(dev);
			            return 1;
					}
					break;
				case CYPRRESS_NO_FW:
					if(devcounter == crusher->devnum) {
						if (anti_loop == 0 && upload_fw) {

						    switch(devices[dev_type_id].devType) {
						        case DEV_TYPE_ENCODER:
						            crusher->fx2fw = FX2Firmware;
						            break;
                                case DEV_TYPE_CAPTURE:
                                    crusher->fx2fw = FX2_CaptureFirmware;
                                    break;
                                case DEV_TYPE_ENCODER_HD:
                                    crusher->fx2fw = FX2_HDFirmware;
                                    break;
                                default:
                                    ERROR("Strange device type %d",devices[dev_type_id].devType);
                                    return 0;
						    }

							/* we have firmware file, upload it! */
						    DEBUG("Opening file %s.", crusher->fx2fw);
						    fx2Fw = fopen(crusher->fx2fw, "r");
							if (!fx2Fw) {
								ERROR("Open file %s failed.", crusher->fx2fw);
								return 0;
							}

							crusher->udev = (void *) crusher_usb_open(dev);
							if( !crusher->udev ) {
								ERROR("Open device (0x%04x:0x%04x) failed.",
										dev->descriptor.idVendor, dev->descriptor.idProduct);
								if(fx2Fw)
									fclose(fx2Fw);
								return 0;
							}

							if (!crusher_fx2loader(crusher->udev, fx2Fw, devices[dev_type_id].devType == DEV_TYPE_ENCODER_HD)) {
								ERROR("Uploading FX2 firmware %s failed.", crusher->fx2fw);
								if(fx2Fw)
									fclose(fx2Fw);
								if(crusher->udev)
									usb_close((usb_dev_handle *) crusher->udev);
								return 0;
							}
							usb_close((usb_dev_handle *) crusher->udev);
						}

						usleep(1000*1000);
						if (anti_loop < MAX_USB_ENCODERS){
							anti_loop += 1;
							goto dev_search;
						} else {
							return 0;
						}
					}
					break;
				default:
					DEBUG("Device #%d Passed.", devcounter);
					break;
			}
			devcounter++;
	    }
    }

    if (anti_loop > 0 && anti_loop < MAX_USB_ENCODERS){
		anti_loop += 1;
		usleep(1000*1000);
		goto dev_search;
	}
    return 0;
}


int crusher_init_device(crusher_t *crusher)
{
	FILE	*mg1264Fw = NULL;
	int ret = 0;
	uint8_t  reg_section = 0x40;
	uint16_t regiter16;
	uint32_t regiter32 = 0;
	int i;

	crusher->status = 0;
	if(!mware_tune_config(crusher)) return 0;

	mg1264Fw = fopen(crusher->mg1264fw, "r");
	if(!mg1264Fw) {
		ERROR("Open file %s failed.", crusher->mg1264fw);
		return 0;
	}

	if (crusher->devtype == DEV_TYPE_CAPTURE) {
	    if(!mg1264_reset_bb(crusher, 0x0117)) return 0;
    } else if (crusher->devtype == DEV_TYPE_ENCODER_HD) {
        if(!mg1264_reset_b1(crusher, 0x0001)) return 0;
        if(!mg1264_reset_bb(crusher, 0x004a)) return 0;
        if(!mg1264_reset_bb(crusher, 0x0046)) return 0;
        if(!mg1264_reset_bb(crusher, 0x0047)) return 0;
        reg_section = 0x7d;
	}

	if(!mg1264_reset(crusher)) return 0;
	if ((crusher->devtype == DEV_TYPE_ENCODER) || (crusher->devtype == DEV_TYPE_ENCODER_HD)) {
	    if(!mg1264_hbrr(crusher, 0x001C, &regiter16)) return 0;
        XTREME("Register 0x001C = %04x (mb:0x1001/hd:0303).", regiter16);
	}

	if (crusher->devtype == DEV_TYPE_ENCODER_HD) {
	    if(!mg1264_hbrw(crusher, 0x0032, 0x0010)) return 0;
	    if(!mg1264_hbrr(crusher, 0x0032, &regiter16)) return 0; //===> 0x0010
        XTREME("Register 0x0032 = %04x (mb:0x0010).", regiter16);
        if(!mg1264_hbrr(crusher, 0x001c, &regiter16)) return 0; //===> 0x0303
	    XTREME("Register 0x001C = %04x (mb:0x0303).", regiter16);
	    if(!mg1264_hbrr(crusher, 0x001b, &regiter16)) return 0; //===> 0x0400
	    XTREME("Register 0x001B = %04x (mb:0x0400).", regiter16);
	    if(!mg1264_hbrr(crusher, 0x001a, &regiter16)) return 0; //===> 0x0003
	    XTREME("Register 0x001A = %04x (mb:0x0003).", regiter16);
	    if(!mg1264_hbrw(crusher, 0x001a, 0x0003)) return 0;
	    if(!mg1264_hbrw(crusher, 0x001b, 0x04dd)) return 0;
	    if(!mg1264_hbrw(crusher, 0x001a, 0x30b0)) return 0;
	    if(!mg1264_hbrw(crusher, 0x001b, 0x05dd)) return 0;
	    if(!mg1264_hbrr(crusher, 0x001b, &regiter16)) return 0; //===> 0x05dd
	    XTREME("Register 0x001b = %04x (mb:0x05dd).", regiter16);
	    if(!mg1264_hbrr(crusher, 0x001A, &regiter16)) return 0; //===> 0x30b4
	    XTREME("Register 0x001A = %04x (mb:0x30b4).", regiter16);

	    if(!mg1264_csrw(crusher, 0x0010, 0x05dd30b4, 0x08, 0x04)) return 0;
	    if(!mg1264_csrw(crusher, 0x0020, 0x00008006, 0x08, 0x04)) return 0;
	    if(!mg1264_csrw(crusher, 0x0020, 0x00009106, 0x08, 0x04)) return 0;

	} else {
	    if(!mg1264_hbrr(crusher, 0x001A, &regiter16)) return 0;
        XTREME("Register 0x001A = %04x (mb:0x0200).", regiter16);
        if(!mg1264_hbrw(crusher, 0x001A, 0x0000)) return 0;

        if(!mg1264_hbrw(crusher, 0x001B, crusher->devmode == DEV_TYPE_CAPTURE ? 0x0064 : 0x005C)) return 0;
        if(!mg1264_hbrr(crusher, 0x001A, &regiter16)) return 0;
        XTREME("Register 0x001A = %04x (mb:0x0000).", regiter16);
        if(!mg1264_hbrw(crusher, 0x001A, 0x0001)) return 0;

        /* init i2c devices */
        if (crusher->devtype == DEV_TYPE_CAPTURE)
            if(!i2c_init(crusher)) return 0;
	}

	ret = mg1264_load_firmware(crusher, mg1264Fw);

	if(mg1264Fw)
		fclose(mg1264Fw);

	if(!ret)
		return ret;

	/* max tryes = 64, if device not initialized, then it fails! */
	i=0;
	do {
		if(!mg1264_mbr(crusher, 0x00000ffc, 0x04, (uint8_t *) &regiter32, reg_section)) return 0;
		usleep(100*1000);
		i++;
	} while(regiter32 == 0 && i < 64);

	regiter32 = be2me_32(regiter32);
	XTREME("regiter0[0ffc] = %08x.", regiter32);
	/* device must return (0x40000000 | gpb)
	 * SD: 0x400c5180
	 * HD: 0x40111070 */
    if( !(regiter32 && 0x40000000))
                 return 0;
	if (crusher->devtype == DEV_TYPE_ENCODER_HD) {
	    uint8_t buff0[20];
        /*20 bytes*/
	    if(!mg1264_mbr (crusher, regiter32/*0x40111070*/, 20, buff0, 0x7d)) return 0;
	    dump_bytes((char *) buff0, 20);
	} else {
        if( !(regiter32 && 0x40000000) || ((regiter32 & 0x3FFFFFFF) != crusher->gpb) )
            return 0;

        /* read status
         * >>0000   b5 04 02 c2 00 04 00
         * <<0000   00 00 00 40
         *  */
        if(!mg1264_csrr(crusher, 0x00C2, &regiter32, 0x04, 0x02)) return 0;
        if (regiter32 != 0x40) {
            ERROR("status=%d (must be 0x40)", regiter32);
            return 0;
        }
	}

    /* >>0000   b6 00 00 00 00 08 00 00 10 0c 00 00 00 02 04 */
    if(!mg1264_csrw(crusher, 0x10, 0x0C, 0x02, 0x04)) return 0;

    //>>0000   ba 10 00 17 00 00 00
    if(!mg1264_hbrw(crusher, 0x17, 0x10)!=0) return 0;
    //>>0000   ba 10 00 18 00 00 00
    if(!mg1264_hbrw(crusher, 0x18, 0x10)!=0) return 0;


    if (crusher->devmode == DEV_TYPE_ENCODER) {
        /*>>0000   bd 00 00 00 00 09 00 00 00 0f fc 00 00 00 04 40
         *<<0000   40 0c 51 80 */
        if(!mg1264_mbr(crusher, 0x00000ffc, 0x04, (uint8_t *) &regiter32, 0x40)) return 0;
        regiter32 = be2me_32(regiter32);
        XTREME("regiter1[0ffc] = %08x.", regiter32);
        /* device must return (0x40000000 | gpb) */
        if( !(regiter32 && 0x40000000) || ((regiter32 & 0x3FFFFFFF) != crusher->gpb) ) {
            ERROR("regiter1[0ffc]: %08x != %08x, %d.", regiter32 & 0x3FFFFFFF, crusher->gpb, (regiter32 && 0x40000000));
            return 0;
        }

        /*>>0000   bd 00 00 00 00 09 00 40 0c 51 80 00 00 00 04 40
         *<<0000   40 0c 51 00
         * */

        uint32_t evSection;
        if(!mg1264_mbr(crusher, regiter32, 0x04, (uint8_t *) &evSection, reg_section)) return 0;

        evSection = be2me_32(evSection);
        XTREME("regiter2[0x%08x] = %08x.", crusher->gpb || 0x40000000, evSection);
        //if( !(regiter32 && 0x40000000) || ((regiter32 && 0x3FFFFFFF) != (crusher->gpb - 0x80)) )
        //return 0;
        /*gpb: 0x000c5180*/
    }

    if (crusher->devmode == DEV_TYPE_ENCODER_HD) {
        /* HD start event here?? */
        exit(0);
    }

    crusher->status = 1;
	return crusher->status;
}



int crusher_send_textconfig(crusher_t *crusher)
{
	uint32_t status;
	assert(crusher->status > 0);
	crusher->status = 0;

	if(!crusher->textcfg) {
	    switch(crusher->devmode){
	        case DEV_TYPE_CAPTURE:
	            if(!encoderCaptureRestart(crusher)) return 0;
	            DEBUG("sending capture textconfig");
	            if(!mg1264_send_capture_textconfig(crusher)) return 0;
	            break;
	        case DEV_TYPE_ENCODER:
	            if(!encoderRestart(crusher)) return 0;
	            DEBUG("sending encoder textconfig");
	            if(!mg1264_send_encoder_textconfig(crusher)) return 0;
                break;
            case DEV_TYPE_ENCODER_HD:
                DEBUG("sending HD encoder textconfig");
                //if(!mg3500_send_encoder_textconfig(crusher)) return 0;
                break;
            default:
                ERROR("unknown device type in crusher_send_textconfig(): %d", crusher->textcfg);
                return 0;
	    }
	} else {
#ifdef CODEC_CLI
		DEBUG("sending textconfig: %s", crusher->textcfg);
		if(!mg1264_cli_parse_textconfig(crusher)) return 0;
#else
		ERROR("libcrusher264 compiled without codec cli support!");
		return 0;
#endif
	}

	if(crusher->devmode != DEV_TYPE_ENCODER_HD) {
        if(!mg1264_csrr(crusher, 0x00C2, &status, 0x04, 0x02)) return 0;
        if (status != 0x40) {
            ERROR("status=%d (must be 0x40)", status);
            return 0;
        }
        XTREME("status=%d", status);
	}
	crusher->status = 1;
	return crusher->status;
}



int	crusher_start(crusher_t *crusher)
{
    assert(crusher->status > 0);
    int ret;
    crusher->status = 0;

    if(crusher->devmode == DEV_TYPE_CAPTURE) {
        i2c_init2(crusher);
        /* 0000   00 00 00 03 00 00 04 00 00 03 00 01 00 00 00 00
         * 0010   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
         * 3, 1024, 0x00030001 */
        if(!RawCmd(crusher, AVENCODER_CTRLOBJ_ID, CMD_OPCODE_SUBSCRIBE_EVENT,
                AVE_EV_BITSTREAM_BLOCK_READY, 0, 0, 0, 0, 0)) return 0;
        /* 0000   00 00 00 03 00 00 00 02 00 00 00 00 00 00 00 00
         * 0010   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
         * 3, 2 */

        ret = encoderRecord(crusher);
        if(!ret) return 0;
        crusher->status = 1;
        return ret;
    } else {
        DEBUG("crusher start");
        if(!crusher->framesToLoad || !crusher->inputFrameLen) {
            ERROR("no frames to load: n:%d, len:%d", crusher->framesToLoad, crusher->inputFrameLen);
            return 0;
        }

        crusher->input_buffer = calloc(crusher->framesToLoad, crusher->inputFrameLen);
        if(!crusher->input_buffer)
            return 0;
        /* PLAY */
        /* 0000   00 00 00 03 00 00 00 0b 00 00 00 01 b0 1a de 8b
         * 0010   00 90 75 38 b0 1a de 48 90 09 05 c8 0e 53 9d 38*/

        ret = encoderPlay(crusher);
        if(!ret) return 0;
        crusher->status = 1;
        return ret;
    }
    return 0;
}



int	crusher_encode(crusher_t *crusher, uint8_t *converted_data, int data_size)
{
	event_t *event = (event_t *) crusher->lastEvent;
	assert(crusher->status > 0);

	if(crusher->event_complete)
		if(!mg1264_event(crusher, event)) return CODEC_FAIL;

	XTREME("event: %08x", event->eventId);

	switch (event->eventId) {
		case AVE_EV_READY:
			XTREME("Encoding timestamp: %d", event->timestamp);
			if(crusher->devmode == DEV_TYPE_ENCODER) {
                if(!crusher->finished){
                    /* send data to device, if no data, finish current event */
                    if (!mg1264_sendframes(crusher, converted_data, data_size)) {
                        ERROR("Error sending frames to device");
                        return CODEC_FAIL;
                    }

                    if(converted_data && data_size) {
                        return CODEC_FRAME_SENT;
                    } else {
                        DEBUG("No more data from source, flushing...");
                        /* 3, 1, */
                        /* 0000   00 00 00 03 00 00 00 01 b0 1a de 44 00 00 00 40
                         * 0010   00 00 03 e8 ff f8 00 00 00 00 00 00 1d cd 65 00*/
                        /* TODO: all args needed? */
                        RawCmd(crusher, 3, 1, 0xb01ade44, 0x40, 0x03e8, 0xfff80000, 0x00, 0x1dcd6500);
                        //encoderFlush(crusher);
                        crusher->finished = 1;
                        crusher->out_blocks = 0;
                    }
                } else {
                    crusher->out_blocks = 0;
                }
			} else {
			    DEBUG("Nothing to do in AVE_EV_READY");
			}
			break;
		case AVE_EV_BITSTREAM_BLOCK_READY:
			XTREME("Encoder Have Data in Buffer");
			if (!mg1264_readframes(crusher)){
				ERROR("Can't read frames from encoder");
				return CODEC_FAIL;
			}
			break;
		case AVE_EV_BITSTREAM_FLUSHED:
		    if(crusher->devmode == DEV_TYPE_CAPTURE) {
		        XTREME("Encoder Have Data in Buffer");
		        if (!mg1264_readframes(crusher)){
		            ERROR("Can't read frames from encoder");
		            return CODEC_FAIL;
		        }
		    } else {
                DEBUG("Encoder Finished");
                mg1264_finish(crusher);
                return CODEC_FINISHED;
		    }
			break;
		case SYSTEM_EV_BAD_IMAGE_DIMENSION:
			ERROR("Bad image dimensions: %dx%d", crusher->width, crusher->height);
			return CODEC_FAIL;
			break;
#if 0
		case AVE_EV_FLUSH_COMPLETE:
			DEBUG("Event 0x%08x for %d: Unknown", event.eventId, event.controlObjectId);
#endif
		default:
			ERROR("Unknown event: 0x%08x objId:%d", event->eventId, event->controlObjectId);
			return CODEC_FAIL;
			break;
	}

	return CODEC_FLUSHED;
}
