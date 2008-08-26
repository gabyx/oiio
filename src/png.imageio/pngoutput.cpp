/////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2008 Larry Gritz
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// 
// (this is the MIT license)
/////////////////////////////////////////////////////////////////////////////


#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <time.h>

#include <pngio.h>

#include <boost/algorithm/string.hpp>
using boost::algorithm::iequals;

#include "dassert.h"
#include "imageio.h"
#include "strutil.h"


using namespace OpenImageIO;


class PNGOutput : public ImageOutput {
public:
    PNGOutput ();
    virtual ~PNGOutput ();
    virtual const char * format_name (void) const { return "png"; }
    virtual bool supports (const char *feature) const;
    virtual bool open (const char *name, const ImageSpec &spec,
                       bool append=false);
    virtual bool close ();
    virtual bool write_scanline (int y, int z, ParamBaseType format,
                                 const void *data, stride_t xstride);
    virtual bool write_tile (int x, int y, int z,
                             ParamBaseType format, const void *data,
                             stride_t xstride, stride_t ystride, stride_t zstride);

private:
    PNG *m_tif;
    std::vector<unsigned char> m_scratch;
    int m_planarconfig;

    // Initialize private members to pre-opened state
    void init (void) {
        m_tif = NULL;
    }

    // Convert planar contiguous to planar separate data format
    void contig_to_separate (int n, const unsigned char *contig,
                             unsigned char *separate);
    // Add a parameter to the output
    bool put_parameter (const std::string &name, ParamType type,
                        const void *data);
};




// Obligatory material to make this a recognizeable imageio plugin:
extern "C" {

DLLEXPORT PNGOutput *png_output_imageio_create () { return new PNGOutput; }

// DLLEXPORT int imageio_version = IMAGEIO_VERSION;   // it's in pngoutput.cpp

DLLEXPORT const char * png_output_extensions[] = {
    "png", "tif", "tx", "env", "sm", "vsm", NULL
};

};



PNGOutput::PNGOutput ()
{
    init ();
}



PNGOutput::~PNGOutput ()
{
    // Close, if not already done.
    close ();
}



bool
PNGOutput::supports (const char *feature) const
{
    if (! strcmp (feature, "tiles"))
        return true;
    if (! strcmp (feature, "multiimage"))
        return true;

    // FIXME: we could support "volumes" and "empty"

    // Everything else, we either don't support or don't know about
    return false;
}



bool
PNGOutput::open (const char *name, const ImageSpec &userspec, bool append)
{
    close ();  // Close any already-opened file
    m_spec = userspec;  // Stash the spec

    // Open the file
    m_tif = PNGOpen (name, append ? "a" : "w");
    if (! m_tif) {
        error ("Can't open \"%s\" for output.", name);
        return false;
    }

    PNGSetField (m_tif, PNGTAG_XPOSITION, (float)m_spec.x);
    PNGSetField (m_tif, PNGTAG_YPOSITION, (float)m_spec.y);
    PNGSetField (m_tif, PNGTAG_IMAGEWIDTH, m_spec.width);
    PNGSetField (m_tif, PNGTAG_IMAGELENGTH, m_spec.height);
    if ((m_spec.full_width != 0 || m_spec.full_height != 0) &&
        (m_spec.full_width != m_spec.width || m_spec.full_height != m_spec.height)) {
        PNGSetField (m_tif, PNGTAG_PIXAR_IMAGEFULLWIDTH, m_spec.full_width);
        PNGSetField (m_tif, PNGTAG_PIXAR_IMAGEFULLLENGTH, m_spec.full_height);
    }
    if (m_spec.tile_width) {
        PNGSetField (m_tif, PNGTAG_TILEWIDTH, m_spec.tile_width);
        PNGSetField (m_tif, PNGTAG_TILELENGTH, m_spec.tile_height);
    } else {
        // Scanline images must set rowsperstrip
        PNGSetField (m_tif, PNGTAG_ROWSPERSTRIP, 32);
    }
    PNGSetField (m_tif, PNGTAG_SAMPLESPERPIXEL, m_spec.nchannels);
    PNGSetField (m_tif, PNGTAG_ORIENTATION, ORIENTATION_TOPLEFT); // always
    
    int bps, sampformat;
    switch (m_spec.format) {
    case PT_INT8:
        bps = 8;
        sampformat = SAMPLEFORMAT_INT;
        break;
    case PT_UINT8:
        bps = 8;
        sampformat = SAMPLEFORMAT_UINT;
        break;
    case PT_INT16:
        bps = 16;
        sampformat = SAMPLEFORMAT_INT;
        break;
    case PT_UINT16:
        bps = 16;
        sampformat = SAMPLEFORMAT_UINT;
        break;
    case PT_HALF:
        // Silently change requests for unsupported 'half' to 'float'
        m_spec.format = PT_FLOAT;
    case PT_FLOAT:
        bps = 32;
        sampformat = SAMPLEFORMAT_IEEEFP;
        break;
    default:
        error ("PNG doesn't support %s images (\"%s\")",
               typestring(m_spec.format), name);
        close();
        return false;
    }
    PNGSetField (m_tif, PNGTAG_BITSPERSAMPLE, bps);
    PNGSetField (m_tif, PNGTAG_SAMPLEFORMAT, sampformat);

    int photo = (m_spec.nchannels > 1 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
    PNGSetField (m_tif, PNGTAG_PHOTOMETRIC, photo);

    if (m_spec.nchannels == 4 && m_spec.alpha_channel == m_spec.nchannels-1) {
        unsigned short s = EXTRASAMPLE_ASSOCALPHA;
        PNGSetField (m_tif, PNGTAG_EXTRASAMPLES, 1, &s);
    }

    // Default to LZW compression if no request came with the user spec
    if (! m_spec.find_attribute("compression"))
        m_spec.attribute ("compression", "lzw");

    ImageIOParameter *param;
    const char *str;

    // Did the user request separate planar configuration?
    m_planarconfig = PLANARCONFIG_CONTIG;
    if ((param = m_spec.find_attribute("planarconfig", PT_STRING)) &&
            (str = *(char **)param->data())) {
        if (! strcmp (str, "separate"))
            m_planarconfig = PLANARCONFIG_SEPARATE;
    }
    PNGSetField (m_tif, PNGTAG_PLANARCONFIG, m_planarconfig);

    // Automatically set date field if the client didn't supply it.
    if (! m_spec.find_attribute("DateTime")) {
        time_t now;
        time (&now);
        struct tm mytm;
        localtime_r (&now, &mytm);
        std::string date = Strutil::format ("%4d:%02d:%02d %2d:%02d:%02d",
                               mytm.tm_year+1900, mytm.tm_mon+1, mytm.tm_mday,
                               mytm.tm_hour, mytm.tm_min, mytm.tm_sec);
        m_spec.attribute ("DateTime", date);
    }

    // Deal with all other params
    for (size_t p = 0;  p < m_spec.extra_attribs.size();  ++p)
        put_parameter (m_spec.extra_attribs[p].name().string(),
                       m_spec.extra_attribs[p].type(),
                       m_spec.extra_attribs[p].data());

    PNGCheckpointDirectory (m_tif);  // Ensure the header is written early

    return true;
}



bool
PNGOutput::put_parameter (const std::string &name, ParamType type,
                           const void *data)
{
    if (iequals(name, "Artist") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_ARTIST, *(char**)data);
        return true;
    }
    if (iequals(name, "Compression") && type == PT_STRING) {
        int compress = COMPRESSION_LZW;  // default
        const char *str = *(char **)data;
        if (str) {
            if (! strcmp (str, "none"))
                compress = COMPRESSION_NONE;
            else if (! strcmp (str, "lzw"))
                compress = COMPRESSION_LZW;
            else if (! strcmp (str, "zip") || ! strcmp (str, "deflate"))
                compress = COMPRESSION_ADOBE_DEFLATE;
        }
        PNGSetField (m_tif, PNGTAG_COMPRESSION, compress);
        // Use predictor when using compression
        if (compress == COMPRESSION_LZW || compress == COMPRESSION_ADOBE_DEFLATE) {
            if (m_spec.format == PT_FLOAT || m_spec.format == PT_DOUBLE || m_spec.format == PT_HALF)
                PNGSetField (m_tif, PNGTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
            else
                PNGSetField (m_tif, PNGTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
        }
    }
    if (iequals(name, "Copyright") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_COPYRIGHT, *(char**)data);
        return true;
    }
    if (iequals(name, "DateTime") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_DATETIME, *(char**)data);
        return true;
    }
    if ((iequals(name, "name") || iequals(name, "DocumentName")) && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_DOCUMENTNAME, *(char**)data);
        return true;
    }
    if (iequals(name,"fovcot") && type == PT_FLOAT) {
        double d = *(float *)data;
        PNGSetField (m_tif, PNGTAG_PIXAR_FOVCOT, d);
        return true;
    }
    if ((iequals(name, "host") || iequals(name, "HostComputer")) && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_HOSTCOMPUTER, *(char**)data);
        return true;
    }
    if ((iequals(name, "description") || iequals(name, "ImageDescription")) &&
          type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_IMAGEDESCRIPTION, *(char**)data);
        return true;
    }
    if (iequals(name, "png:Predictor") && type == PT_INT) {
        PNGSetField (m_tif, PNGTAG_PREDICTOR, *(int *)data);
        return true;
    }
    if (iequals(name, "ResolutionUnit") && type == PT_STRING) {
        const char *s = *(char**)data;
        bool ok = true;
        if (! strcmp (s, "none"))
            PNGSetField (m_tif, PNGTAG_RESOLUTIONUNIT, RESUNIT_NONE);
        else if (! strcmp (s, "in") || ! strcmp (s, "inch"))
            PNGSetField (m_tif, PNGTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        else if (! strcmp (s, "cm"))
            PNGSetField (m_tif, PNGTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER);
        else ok = false;
        return ok;
    }
    if (iequals(name, "ResolutionUnit") && type == PT_UINT) {
        PNGSetField (m_tif, PNGTAG_RESOLUTIONUNIT, *(unsigned int *)data);
        return true;
    }
    if (iequals(name, "png:RowsPerStrip")) {
        if (type == PT_INT) {
            PNGSetField (m_tif, PNGTAG_ROWSPERSTRIP, *(int*)data);
            return true;
        } else if (type == PT_STRING) {
            // Back-compatibility with Entropy and PRMan
            PNGSetField (m_tif, PNGTAG_ROWSPERSTRIP, atoi(*(char **)data));
            return true;
        }
    }
    if (iequals(name, "Software") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_SOFTWARE, *(char**)data);
        return true;
    }
    if (iequals(name, "png:SubFileType") && type == PT_INT) {
        PNGSetField (m_tif, PNGTAG_SUBFILETYPE, *(int*)data);
        return true;
    }
    if (iequals(name, "textureformat") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_PIXAR_TEXTUREFORMAT, *(char**)data);
        return true;
    }
    if (iequals(name, "wrapmodes") && type == PT_STRING) {
        PNGSetField (m_tif, PNGTAG_PIXAR_WRAPMODES, *(char**)data);
        return true;
    }
    if (iequals(name, "worldtocamera") && type == PT_MATRIX) {
        PNGSetField (m_tif, PNGTAG_PIXAR_MATRIX_WORLDTOCAMERA, data);
        return true;
    }
    if (iequals(name, "worldtoscreen") && type == PT_MATRIX) {
        PNGSetField (m_tif, PNGTAG_PIXAR_MATRIX_WORLDTOSCREEN, data);
        return true;
    }
    if (iequals(name, "XResolution") && type == PT_FLOAT) {
        PNGSetField (m_tif, PNGTAG_XRESOLUTION, *(float *)data);
        return true;
    }
    if (iequals(name, "YResolution") && type == PT_FLOAT) {
        PNGSetField (m_tif, PNGTAG_YRESOLUTION, *(float *)data);
        return true;
    }
    return false;
}



bool
PNGOutput::close ()
{
    if (m_tif)
        PNGClose (m_tif);
    init ();      // re-initialize
    return true;  // How can we fail?
}



/// Helper: Convert n pixels from contiguous (RGBRGBRGB) to separate
/// (RRRGGGBBB) planarconfig.
void
PNGOutput::contig_to_separate (int n, const unsigned char *contig,
                                unsigned char *separate)
{
    int channelbytes = m_spec.channel_bytes();
    for (int p = 0;  p < n;  ++p)                     // loop over pixels
        for (int c = 0;  c < m_spec.nchannels;  ++c)    // loop over channels
            for (int i = 0;  i < channelbytes;  ++i)  // loop over data bytes
                separate[(c*n+p)*channelbytes+i] =
                    contig[(p*m_spec.nchannels+c)*channelbytes+i];
}



bool
PNGOutput::write_scanline (int y, int z, ParamBaseType format,
                            const void *data, stride_t xstride)
{
    m_spec.auto_stride (xstride, format, spec().nchannels);
    const void *origdata = data;
    data = to_native_scanline (format, data, xstride, m_scratch);

    y -= m_spec.y;
    if (m_planarconfig == PLANARCONFIG_SEPARATE && m_spec.nchannels > 1) {
        // Convert from contiguous (RGBRGBRGB) to separate (RRRGGGBBB)
        m_scratch.resize (m_spec.scanline_bytes());
        contig_to_separate (m_spec.width, (const unsigned char *)data, &m_scratch[0]);
        PNGWriteScanline (m_tif, &m_scratch[0], y);
    } else {
        // No contig->separate is necessary.  But we still use scratch
        // space since PNGWriteScanline is destructive when
        // PNGTAG_PREDICTOR is used.
        if (data == origdata) {
            m_scratch.assign ((unsigned char *)data,
                              (unsigned char *)data+m_spec.scanline_bytes());
            data = &m_scratch[0];
        }
        PNGWriteScanline (m_tif, (tdata_t)data, y);
    }
    // Every 16 scanlines, checkpoint (write partial file)
    if ((y % 16) == 0)
        PNGCheckpointDirectory (m_tif);

    return true;
}



bool
PNGOutput::write_tile (int x, int y, int z,
                        ParamBaseType format, const void *data,
                        stride_t xstride, stride_t ystride, stride_t zstride)
{
    m_spec.auto_stride (xstride, ystride, zstride, format, spec().nchannels,
                        spec().tile_width, spec().tile_height);
    x -= m_spec.x;   // Account for offset, so x,y are file relative, not 
    y -= m_spec.y;   // image relative
    const void *origdata = data;   // Stash original pointer
    data = to_native_tile (format, data, xstride, ystride, zstride, m_scratch);
    if (m_planarconfig == PLANARCONFIG_SEPARATE && m_spec.nchannels > 1) {
        // Convert from contiguous (RGBRGBRGB) to separate (RRRGGGBBB)
        int tile_pixels = m_spec.tile_width * m_spec.tile_height 
                            * std::max (m_spec.tile_depth, 1);
        int plane_bytes = tile_pixels * typesize(m_spec.format);
        DASSERT (plane_bytes*m_spec.nchannels == m_spec.tile_bytes());
        m_scratch.resize (m_spec.tile_bytes());
        contig_to_separate (tile_pixels, (const unsigned char *)data, &m_scratch[0]);
        for (int c = 0;  c < m_spec.nchannels;  ++c)
            PNGWriteTile (m_tif, (tdata_t)&m_scratch[plane_bytes*c], x, y, z, c);
    } else {
        // No contig->separate is necessary.  But we still use scratch
        // space since PNGWriteTile is destructive when
        // PNGTAG_PREDICTOR is used.
        if (data == origdata) {
            m_scratch.assign ((unsigned char *)data,
                              (unsigned char *)data + m_spec.tile_bytes());
            data = &m_scratch[0];
        }
        PNGWriteTile (m_tif, (tdata_t)data, x, y, z, 0);
    }

    // Every row of tiles, checkpoint (write partial file)
    if ((y % m_spec.tile_height) == 0)
        PNGCheckpointDirectory (m_tif);

    return true;
}
