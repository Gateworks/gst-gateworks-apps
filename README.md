# gst-gateworks-apps #

These suite of programs are examples written by Gateworks in order to help developers with specific types of applications.

## Compatibility ##

These programs are made in mind to run on the i.mx6 platform.

Also, full Yocto SDK of an image containing the following packages: gstreamer-1.0, and glib-2.0 (accomplished by bitbaking with a `-cpopulate_sdk` flag on the image recipe)

## Cross Compilation ##

To cross compile, please use the script `make-for-imx6`. This script configures the environment as required and builds the target.

If using `make-for-imx6` , please make sure that your toolchain exists at `/opt/poky/1.8`. If it does not, you may change it via environmental variable `SDK_LOC`. If your toolchain differs from the default, you may change it via environmental variable `SDK_NAME`.

## Target Compilation ##

If compiling on the target device, just type `make`


----------


# gst-variable-rtsp-server #

This program acts as a RTSP server to provide a live-stream from a v4l2 video source of the users choosing. A nifty feature of this program is its' ability to dynamically change the bitrate of the video stream on the fly depending on the number of clients connected at any given time. This allows for better usage of network bandwidth when required.

The basic pipeline that this program uses is as follows: `v4l2src ! imxipuvideotransform ! imxvpuenc_h264 ! rtph264pay`

## Requirements ##

This program uses gstreamer elements provided by [gstreamer-imx](https://github.com/Freescale/gstreamer-imx) and gstreamer-rtsp-server-1.0. Before running this program, please verify that these plugins are available.

## Compile ##

To cross compile: `./make-for-imx6 gst-variable-rtsp-server`

To target compile: `make gst-variable-rtsp-server`


## Usage ##
For the latest help, please run `gst-variable-rtsp-server --help`

As of this writing, the usage is as follows:

```
Usage: gst-variable-rtsp-server [OPTIONS]

Options:
 --help,            -? - This usage
 --version,         -v - Program Version: 1.1
 --debug,           -d - Debug Level (default: 0)
 --mount-point,     -m - What URI to mount (default: /stream)
 --port,            -p - Port to sink on (default: 9099)
 --user-pipeline,   -u - User supplied pipeline. Note the
                         below options are NO LONGER
                         applicable.
 --src-element,     -s - Gstreamer source element. Must have
                         a 'device' property (default: v4l2src)
 --video-in,        -i - Input Device (default: /dev/video0)
 --caps-filter,     -f - Caps filter between src and
                         video transform (default: None)
 --max-bitrate,     -b - Max allowable bitrate (default: 10000)
 --min-bitrate,        - Min allowable bitrate (default: 0)
 --max-quant-lvl,      - Max Quant-Level (default: 51)
 --min-quant-lvl,   -l - Min Quant-Level (default: 0)
 --config-interval, -c - Interval to send rtp config (default: 2s)
 --idr              -a - Interval between IDR Frames (default: 0)
 --msg-rate,        -r - Rate of messages displayed (default: 5s)

Examples:
 1. Capture using imxv4l2videosrc, changes quality:
        gst-variable-rtsp-server -s imxv4l2videosrc

 2. Create RTSP server out of user created pipeline:
        gst-variable-rtsp-server -u "videotestsrc ! imxvpuenc_h264 ! rtph264pay pt=96"
```
