/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */
//
// TXImage.cxx
//

#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <list>
#include <rfb/TransImageGetter.h>
#include <rfb/Exception.h>
#include <rfb/LogWriter.h>
#include "TXWindow.h"
#include "TXImage.h"
#include <cairo.h>
#include <cairo-xlib.h>
using namespace rfb;

static rfb::LogWriter vlog("TXImage");

TXImage::TXImage(Display* d, int width, int height, Visual* vis_, int depth_)
  : xim(0), dpy(d), vis(vis_), depth(depth_), shminfo(0), tig(0), cube(0), inited(false)
{
  width_ = width;
  height_ = height;
    w_scale_rate = (double)w_scaled/width_;
    h_scale_rate = (double)h_scaled/height_;
  for (int i = 0; i < 256; i++)
    colourMap[i].r = colourMap[i].g = colourMap[i].b = 0;

  if (!vis)
    vis = DefaultVisual(dpy,DefaultScreen(dpy));
  if (!depth)
    depth = DefaultDepth(dpy,DefaultScreen(dpy));

  createXImage();
  getNativePixelFormat(vis, depth);
  colourmap = this;
    xrformat = XRenderFindVisualFormat(dpy, vis);
    xform = { {
                      { XDoubleToFixed(1/w_scale_rate), XDoubleToFixed(0), XDoubleToFixed(0) },
                      { XDoubleToFixed(0), XDoubleToFixed(1/h_scale_rate), XDoubleToFixed(0) },
                      { XDoubleToFixed(0), XDoubleToFixed(0), XDoubleToFixed(1) }
              }
    };

  format.bpp = 0;  // just make it different to any valid format, so that...
  setPF(nativePF); // ...setPF() always works


//  fprintf(stderr, "TED__TXImage::TXImage --> create pixmap and picture ori(%d, %d) -- scaled(%d, %d)\n",
//          width_, height_, w_scaled, h_scaled);

}

TXImage::~TXImage()
{
  if (data != (rdr::U8*)xim->data) delete [] data;
  destroyXImage();
  delete tig;
  delete cube;
  XFreePixmap(dpy, pixmap_src);
  XFreePixmap(dpy, pixmap_dst);
  XRenderFreePicture(dpy, picture_src);
  XRenderFreePicture(dpy, picture_dst);
}

void TXImage::resize(int w, int h)
{
  fprintf(stderr, "TED__TXImage::resize --> input(%d, %d)\n", w, h);
  if (w == width() && h == height()) return;

  int oldStrideBytes = getStride() * (format.bpp/8);
  int rowsToCopy = __rfbmin(h, height());
  int bytesPerRow = __rfbmin(w, width()) * (format.bpp/8);
  rdr::U8* oldData = 0;
  bool allocData = false;

  if (data != (rdr::U8*)xim->data) {
    oldData = (rdr::U8*)data;
    allocData = true;
  } else {
    oldData = new rdr::U8[xim->bytes_per_line * height()];
    memcpy(oldData, xim->data, xim->bytes_per_line * height());
  }

  destroyXImage();
  width_ = w;
  height_ = h;
  createXImage();

  if (allocData)
    data = new rdr::U8[width() * height() * (format.bpp/8)];
  else
    data = (rdr::U8*)xim->data;

  int newStrideBytes = getStride() * (format.bpp/8);
  for (int i = 0; i < rowsToCopy; i++)
    memcpy((rdr::U8*)data + newStrideBytes * i, oldData + oldStrideBytes * i,
           bytesPerRow);
  delete [] oldData;
}

void TXImage::setPF(const PixelFormat& newPF)
{
  if (newPF.equal(format)) return;
  format = newPF;

  if (data != (rdr::U8*)xim->data) delete [] data;
  delete tig;
  tig = 0;

  if (format.equal(nativePF) && format.trueColour) {
    data = (rdr::U8*)xim->data;
  } else {
    data = new rdr::U8[width() * height() * (format.bpp/8)];
    tig = new TransImageGetter();
    tig->init(this, nativePF, 0, cube);
  }
}

int TXImage::getStride() const
{
  if (data == (rdr::U8*)xim->data)
    return xim->bytes_per_line / (xim->bits_per_pixel / 8);
  else
    return width();
}

void TXImage::put(Window win, GC gc, const rfb::Rect& r)
{
  if (r.is_empty()) return;
  int x = r.tl.x;
  int y = r.tl.y;
  int w = r.width();
  int h = r.height();

  if (data != (rdr::U8*)xim->data) {
    rdr::U8* ximDataStart = ((rdr::U8*)xim->data + y * xim->bytes_per_line
                             + x * (xim->bits_per_pixel / 8));
    tig->getImage(ximDataStart, r,
                  xim->bytes_per_line / (xim->bits_per_pixel / 8));
  }
    // origin
    // XPutImage(dpy, win, gc, xim, x, y, x ,y, w, h);

    int x_dst = (int)(x * w_scale_rate);
    int y_dst = (int)(y * h_scale_rate);
    unsigned int w_dst = (unsigned int)(w * w_scale_rate);
    unsigned int h_dst = (unsigned int)(h * h_scale_rate);
    w_dst = w_dst < 1 ? 1 : w_dst;
    h_dst = h_dst < 1 ? 1 : h_dst;

//    fprintf(stderr, "\nTED__TXImage::put --> r(%d, %d, %d, %d)"\
//    "..r_dst(%d, %d, %d, %d)\n", x, y, w, h, x_dst, y_dst, w_dst, h_dst);

    pixmap_src = XCreatePixmap(dpy, win, w, h, xrformat->depth);
    XPutImage(dpy, pixmap_src, gc, xim, x, y, 0 ,0, w, h);
    picture_src = XRenderCreatePicture(dpy, pixmap_src, xrformat, 0, NULL);

    pixmap_dst = XCreatePixmap(dpy, win, w_dst, h_dst, xrformat->depth);
    picture_dst = XRenderCreatePicture(dpy, pixmap_dst, xrformat, 0, NULL);

    XRenderSetPictureTransform(dpy, picture_src, &xform);
    XRenderSetPictureFilter(dpy, picture_src, FilterBest, NULL, 0);

    XRenderComposite(dpy,
                     PictOpSrc,
                     picture_src,
                     None,
                     picture_dst,
                     0,
                     0,
                     0,
                     0,
                     0,
                     0,
                     w_dst,
                     h_dst);

    XCopyArea(dpy,
              pixmap_dst,
              win,
              gc,
              0,
              0,
              w_dst,
              h_dst,
              x_dst,
              y_dst);
}

void TXImage::setColourMapEntries(int firstColour, int nColours, rdr::U16* rgbs)
{
  for (int i = 0; i < nColours; i++) {
    colourMap[firstColour+i].r = rgbs[i*3];
    colourMap[firstColour+i].g = rgbs[i*3+1];
    colourMap[firstColour+i].b = rgbs[i*3+2];
  }
}

void TXImage::updateColourMap()
{
  tig->setColourMapEntries(0, 0, 0);
}

void TXImage::lookup(int index, int* r, int* g, int* b)
{
  *r = colourMap[index].r;
  *g = colourMap[index].g;
  *b = colourMap[index].b;
}


static bool caughtError = false;

static int XShmAttachErrorHandler(Display *dpy, XErrorEvent *error)
{
  caughtError = true;
  return 0;
}

class TXImageCleanup {
public:
  std::list<TXImage*> images;
  ~TXImageCleanup() {
    while (!images.empty())
      delete images.front();
  }
};

static TXImageCleanup imageCleanup;

void TXImage::createXImage()
{
  if (XShmQueryExtension(dpy)) {
    shminfo = new XShmSegmentInfo;

    xim = XShmCreateImage(dpy, vis, depth, ZPixmap,
                          0, shminfo, width(), height());

    if (xim) {
      shminfo->shmid = shmget(IPC_PRIVATE,
                              xim->bytes_per_line * xim->height,
                              IPC_CREAT|0777);

      if (shminfo->shmid != -1) {
        shminfo->shmaddr = xim->data = (char*)shmat(shminfo->shmid, 0, 0);

        if (shminfo->shmaddr != (char *)-1) {

          shminfo->readOnly = False;

          XErrorHandler oldHdlr = XSetErrorHandler(XShmAttachErrorHandler);
          XShmAttach(dpy, shminfo);
          XSync(dpy, False);
          XSetErrorHandler(oldHdlr);

          if (!caughtError) {
            vlog.debug("Using shared memory XImage");
            imageCleanup.images.push_back(this);
            return;
          }

          shmdt(shminfo->shmaddr);
        } else {
          vlog.error("shmat failed");
          perror("shmat");
        }

        shmctl(shminfo->shmid, IPC_RMID, 0);
      } else {
        vlog.error("shmget failed");
        perror("shmget");
      }

      XDestroyImage(xim);
      xim = 0;
    } else {
      vlog.error("XShmCreateImage failed");
    }

    delete shminfo;
    shminfo = 0;
  }

  xim = XCreateImage(dpy, vis, depth, ZPixmap,
                     0, 0, width(), height(), BitmapPad(dpy), 0);

  xim->data = (char*)malloc(xim->bytes_per_line * xim->height);
  if (!xim->data) {
    vlog.error("malloc failed");
    exit(1);
  }
}

void TXImage::destroyXImage()
{
  if (shminfo) {
    vlog.debug("Freeing shared memory XImage");
    shmdt(shminfo->shmaddr);
    shmctl(shminfo->shmid, IPC_RMID, 0);
    delete shminfo;
    shminfo = 0;
    imageCleanup.images.remove(this);
  }
  // XDestroyImage() will free(xim->data) if appropriate
  if (xim) XDestroyImage(xim);
  xim = 0;
}


static bool supportedBPP(int bpp) {
  return (bpp == 8 || bpp == 16 || bpp == 32);
}

static int depth2bpp(Display* dpy, int depth)
{
  int nformats;
  XPixmapFormatValues* format = XListPixmapFormats(dpy, &nformats);

  int i;
  for (i = 0; i < nformats; i++)
    if (format[i].depth == depth) break;

  if (i == nformats || !supportedBPP(format[i].bits_per_pixel))
    throw rfb::Exception("Error: couldn't find suitable pixmap format");

  int bpp = format[i].bits_per_pixel;
  XFree(format);
  return bpp;
}

void TXImage::getNativePixelFormat(Visual* vis, int depth)
{
  cube = 0;
  nativePF.depth = depth;
  nativePF.bpp = depth2bpp(dpy, depth);
  nativePF.bigEndian = (ImageByteOrder(dpy) == MSBFirst);
  nativePF.trueColour = (vis->c_class == TrueColor);

  vlog.info("Using default colormap and visual, %sdepth %d.",
            (vis->c_class == TrueColor) ? "TrueColor, " :
            ((vis->c_class == PseudoColor) ? "PseudoColor, " : ""),
            depth);

  if (nativePF.trueColour) {

    nativePF.redShift   = ffs(vis->red_mask)   - 1;
    nativePF.greenShift = ffs(vis->green_mask) - 1;
    nativePF.blueShift  = ffs(vis->blue_mask)  - 1;
    nativePF.redMax   = vis->red_mask   >> nativePF.redShift;
    nativePF.greenMax = vis->green_mask >> nativePF.greenShift;
    nativePF.blueMax  = vis->blue_mask  >> nativePF.blueShift;

  } else {

    XColor xc[256];
    cube = new rfb::ColourCube(6,6,6);
    int r;
    for (r = 0; r < cube->nRed; r++) {
      for (int g = 0; g < cube->nGreen; g++) {
        for (int b = 0; b < cube->nBlue; b++) {
          int i = (r * cube->nGreen + g) * cube->nBlue + b;
          xc[i].red =   r * 65535 / (cube->nRed-1);
          xc[i].green = g * 65535 / (cube->nGreen-1);
          xc[i].blue =  b * 65535 / (cube->nBlue-1);
        }
      }
    }

    TXWindow::getColours(dpy, xc, cube->size());

    for (r = 0; r < cube->nRed; r++) {
      for (int g = 0; g < cube->nGreen; g++) {
        for (int b = 0; b < cube->nBlue; b++) {
          int i = (r * cube->nGreen + g) * cube->nBlue + b;
          cube->set(r, g, b, xc[i].pixel);
        }
      }
    }
  }
}
// scale xim to xim_scaled
void TXImage::scaleXImage(Window win, GC gc,
                          int x_src, int y_src, int x_dst, int y_dst, int w_src, int h_src,
                          int *x_scaled_src, int *y_scaled_src, int *x_scaled_dst, int *y_scaled_dst, int *w_dst, int *h_dst)
{
//  if(!inited)
//  {
//    rect_to_wrap = {0};
//    fprintf(stderr, "TED__TXImage::scaleXImage --> init to create pixmap and picture ori(%d, %d) -- scaled(%d, %d)"\
//                        " -- rate(%lf, %lf)\n", width_, height_, w_scaled, h_scaled, w_scale_rate, h_scale_rate);
//    XRenderPictFormat* format = XRenderFindVisualFormat(dpy, vis);
//    pixmap_src = XCreatePixmap(dpy, win, width_, height_, format->depth);
//    picture_src = XRenderCreatePicture(dpy, pixmap_src, format, 0, NULL);
//    pixmap_dst = XCreatePixmap(dpy, win, w_scaled, h_scaled, format->depth);
//    picture_dst = XRenderCreatePicture(dpy, pixmap_dst, format, 0, NULL);
//
//    XTransform xform = { {
//                               { XDoubleToFixed(1/w_scale_rate), XDoubleToFixed(0), XDoubleToFixed(0) },
//                               { XDoubleToFixed(0), XDoubleToFixed(1/h_scale_rate), XDoubleToFixed(0) },
//                               { XDoubleToFixed(0), XDoubleToFixed(0), XDoubleToFixed(1) }
//                       }
//    };
//    XRenderSetPictureTransform(dpy, picture_src, &xform);
//    XRenderSetPictureFilter(dpy, picture_src, FilterBest, NULL, 0);
//
//    gettimeofday(&tv, NULL);
//    tv_end = {0};
//    inited = true;
//
//
//  }
    addRectToWrapRect(x_src, y_src, w_src, h_src);
    XPutImage(dpy, pixmap_src, gc, xim, x_src, y_src, x_dst, y_dst, w_src, h_src);


    fprintf(stderr, "TED__TXImage::scaleXImage ----> tv_end(%d)-tv(%d)=(%d)\n",
             tv_end.tv_usec, tv.tv_usec, (tv_end.tv_usec - tv.tv_usec));

    gettimeofday(&tv_end, NULL);

    static int draw = 1;
    static int mod = 20;
    static int k = 1;

    if ((tv_end.tv_usec - tv.tv_usec) >= 30 * 1000 || (tv.tv_usec - tv_end.tv_usec) >= 30 * 1000)
    {
        fprintf(stderr, "TED__TXImage::scaleXImage ----> "\
        "rect_to_wrap(x_start=%d, y_start=%d, w=%d, h=%d, x_end=%d, y=end%d)\n",
                rect_to_wrap.x_left_top,
                rect_to_wrap.y_left_top,
                rect_to_wrap.w,
                rect_to_wrap.h,
                rect_to_wrap.x_right_bottom,
                rect_to_wrap.y_right_bottom);

        WrapRect rect_to_scale = getRectToScale();
        WrapRect rect_scaled = getScaledRect(&rect_to_scale);
        WrapRect rect_to_draw = getScaledRect(&rect_to_wrap);

        fprintf(stderr, "TED__TXImage::scaleXImage ----> "\
        "rect_to_scale(x_start=%d, y_start=%d, w=%d, h=%d, x_end=%d, y=end%d)\n",
                rect_to_scale.x_left_top,
                rect_to_scale.y_left_top,
                rect_to_scale.w,
                rect_to_scale.h,
                rect_to_scale.x_right_bottom,
                rect_to_scale.y_right_bottom);

        fprintf(stderr, "TED__TXImage::scaleXImage ----> "\
        "rect_scaled(x_start=%d, y_start=%d, w=%d, h=%d, x_end=%d, y=end%d)\n",
                rect_scaled.x_left_top,
                rect_scaled.y_left_top,
                rect_scaled.w,
                rect_scaled.h,
                rect_scaled.x_right_bottom,
                rect_scaled.y_right_bottom);

        fprintf(stderr, "TED__TXImage::scaleXImage ----> "\
        "rect_to_draw(x_start=%d, y_start=%d, w=%d, h=%d, x_end=%d, y=end%d)\n",
                rect_to_draw.x_left_top,
                rect_to_draw.y_left_top,
                rect_to_draw.w,
                rect_to_draw.h,
                rect_to_draw.x_right_bottom,
                rect_to_draw.y_right_bottom);
        XRenderComposite(dpy,
                         PictOpSrc,
                         picture_src,
                         None,
                         picture_dst,
                         rect_to_scale.x_left_top,
                         rect_to_scale.y_left_top,
                         0,
                         0,
                         rect_scaled.x_left_top,
                         rect_scaled.y_left_top,
                         rect_scaled.w,
                         rect_scaled.h);
        //fprintf(stderr, "TED__TXImage::XCopyArea(%d.%d)...%d\n", w_scaled, h_scaled, k);
        fprintf(stderr, "TED__TXImage::XCopyArea -----------------> wrap_r(%d, %d, %d, %d:  %d, %d)\n",
                rect_scaled.x_left_top,
                rect_scaled.y_left_top,
                rect_scaled.w,
                rect_scaled.h,
                rect_scaled.x_left_top,
                rect_scaled.y_left_top);
//        if(rect_scaled.y_right_bottom > 560)
//           fprintf(stderr, "TED__ ########## >>>>>560:(%d)\n\n\n\n\n\n\n\n", rect_scaled.y_right_bottom);
//        XCopyArea(dpy,
//                  pixmap_dst,
//                  win,
//                  gc,
//                  rect_scaled.x_left_top,
//                  rect_scaled.y_left_top,
//                  rect_scaled.w,
//                  rect_scaled.h,
//                  rect_scaled.x_left_top,
//                  rect_scaled.y_left_top);


        XCopyArea(dpy,
                  pixmap_dst,
                  win,
                  gc,
                  0,
                  0,
                  w_scaled,
                  h_scaled,
                  0,
                  0);
        draw = 1;
        rect_to_wrap = {0};
        tv = tv_end;
    }



  //XCopyArea(dpy, pixmap_dst, win, gc, x_scaled_src, y_scaled_src, w_scaled, h_scaled, x_scaled_dst, y_scaled_dst);
//  gettimeofday(&tv, &tz);
//  fprintf(stderr, "TED__TXImage::scaleXImage finish composite at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);


//  xim_scaled = XGetImage(dpy,
//                            dst_pixmap,
//                            0,
//                            0,
//                            w_scaled,
//                            h_scaled,
//                            AllPlanes, ZPixmap);
//
//  gettimeofday(&tv, &tz);
//  fprintf(stderr, "TED__TXImage::scaleXImage finish XCopyArea at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);



//  gettimeofday(&tv, &tz);
//  fprintf(stderr, "TED__TXImage::scaleXImage finish cleanup at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);
}

void TXImage::scaleXImageCairo(Window win, GC gc, int x_src, int y_src, int x_dst, int y_dst, int w_src, int h_src)
{

  int mod = 60;

  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv, &tz);
  fprintf(stderr, "TED__TXImage::scaleXImageCairo begin at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);

  XRenderPictFormat* format = XRenderFindVisualFormat(dpy, vis);
  Pixmap src_pixmap = XCreatePixmap(dpy,
                                    win,
                                    width_,
                                    height_,
                                    format->depth);

  gettimeofday(&tv, &tz);
  fprintf(stderr, "TED__TXImage::scaleXImageCairo finish createPixmap at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);

  XPutImage(dpy, src_pixmap, gc, xim, x_src, y_src, x_dst, y_dst, w_src, h_src);

  gettimeofday(&tv, &tz);
  fprintf(stderr, "TED__TXImage::scaleXImageCairo finish putimage at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);

  cairo_surface_t *sfc_src = cairo_xlib_surface_create(dpy, src_pixmap, vis, width_, height_);
  cairo_surface_t *sfc_dst = cairo_xlib_surface_create(dpy, win, vis, w_scaled, h_scaled);
  cairo_t *ct = cairo_create(sfc_dst);
  cairo_scale(ct, (double)w_scaled/width_, (double)h_scaled/height_);
  cairo_set_source_surface(ct, sfc_src, 0, 0);
  cairo_paint(ct);

  gettimeofday(&tv, &tz);
  fprintf(stderr, "TED__TXImage::scaleXImageCairo finish cairo at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);

  XFreePixmap(dpy, src_pixmap);
  cairo_surface_destroy(sfc_src);
  cairo_surface_destroy(sfc_dst);
  cairo_destroy(ct);

  gettimeofday(&tv, &tz);
  fprintf(stderr, "TED__TXImage::scaleXImageCairo finish cleanup at(%d.%d)\n", tv.tv_sec%mod, tv.tv_usec);
}

void TXImage::addRectToWrapRect(int x_, int y_, int w_, int h_)
{
   // fprintf(stderr, "TED__TXImage::addRectToWrapRect --> r(%d, %d, %d, %d)\n", x_, y_, w_, h_);
    int x_right_bottom_ = x_ + w_;
    int y_right_bottom_ = y_ + h_;

    // add the first rect
    if(rect_to_wrap.y_right_bottom == 0)
    {
        rect_to_wrap.x_left_top = x_;
        rect_to_wrap.y_left_top = y_;
        rect_to_wrap.x_right_bottom = x_right_bottom_;
        rect_to_wrap.y_right_bottom = y_right_bottom_;
    }
    else
    {
        // calculate the wrapped rect size
        rect_to_wrap.x_left_top = x_ < rect_to_wrap.x_left_top ? x_ : rect_to_wrap.x_left_top;
        rect_to_wrap.y_left_top = y_ < rect_to_wrap.y_left_top ? y_ : rect_to_wrap.y_left_top;
        rect_to_wrap.x_right_bottom = x_right_bottom_ > rect_to_wrap.x_right_bottom ? x_right_bottom_ : rect_to_wrap.x_right_bottom;
        rect_to_wrap.y_right_bottom = y_right_bottom_ > rect_to_wrap.y_right_bottom ? y_right_bottom_ : rect_to_wrap.y_right_bottom;
    }
    //fprintf(stderr, "TED__TXImage::addRectToWrapRect --> wrap_r(%d, %d, %d, %d)\n\n",
//            rect_to_wrap.x_left_top,
//            rect_to_wrap.y_left_top,
//            rect_to_wrap.x_right_bottom,
//            rect_to_wrap.y_right_bottom);
}

WrapRect TXImage::getRectToScale()
{
//    //fprintf(stderr, "TED__TXImage::getRectToScale --> wrap_r(%d, %d, %d, %d)\n",
//            rect_to_wrap.x_left_top,
//            rect_to_wrap.y_left_top,
//            rect_to_wrap.x_right_bottom,
//            rect_to_wrap.y_right_bottom);
    //extend 'rect_to_wrap' ${extend} pix.
    WrapRect ret_rect = {0};
    ret_rect.x_left_top = (rect_to_wrap.x_left_top - extend) > 0 ? (rect_to_wrap.x_left_top - extend) : 0;
    ret_rect.y_left_top = (rect_to_wrap.y_left_top - extend) > 0 ? (rect_to_wrap.y_left_top - extend) : 0;
    ret_rect.x_right_bottom = (rect_to_wrap.x_right_bottom + extend) < width_ ?  (rect_to_wrap.x_right_bottom + extend) :width_;
    ret_rect.y_right_bottom = (rect_to_wrap.y_right_bottom + extend) < height_ ?  (rect_to_wrap.y_right_bottom + extend) :height_;

   // fprintf(stderr, "TED__TXImage::getRectToScale --> to_scale_r(%d, %d, %d, %d)\n\n",
//            ret_rect.x_left_top,
//            ret_rect.y_left_top,
//            ret_rect.x_right_bottom,
//            ret_rect.y_right_bottom);

    return ret_rect;
}

WrapRect TXImage::getScaledRect(WrapRect *ptr_rect) const
{
    int w_ = ptr_rect->x_right_bottom - ptr_rect->x_left_top;
    int h_ = ptr_rect->y_right_bottom - ptr_rect->y_left_top;

    //fprintf(stderr, "TED__TXImage::getScaledRect --> ptr_rect(%d, %d,%d, %d)\n",
//            ptr_rect-> x_left_top,
//            ptr_rect-> y_left_top,
//            w_,
//            h_);


    WrapRect ret_rect = {0};
    ret_rect.x_left_top = (int)(ptr_rect->x_left_top * w_scale_rate);
    ret_rect.y_left_top = (int) (ptr_rect->y_left_top * h_scale_rate);
    ret_rect.w = (unsigned int) (w_ * w_scale_rate);
    ret_rect.h = (unsigned int) (h_ * h_scale_rate);

   // fprintf(stderr, "TED__TXImage::getScaledRect --> scaled_r(%d, %d:  %d, %d)\n",
//            ret_rect.x_left_top,
//            ret_rect.y_left_top,
//            ret_rect.w,
//            ret_rect.h);
    return ret_rect;
}






