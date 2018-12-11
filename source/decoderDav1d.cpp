/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "decoderDav1d.h"

#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include "typedef.h"

// Debug the decoder ( 0:off 1:interactive deocder only 2:caching decoder only 3:both)
#define DECODERDAV1D_DEBUG_OUTPUT 0
#if DECODERDAV1D_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#if DECODERDAV1D_DEBUG_OUTPUT == 1
#define DEBUG_DAV1D if(!isCachingDecoder) qDebug
#elif DECODERDAV1D_DEBUG_OUTPUT == 2
#define DEBUG_DAV1D if(isCachingDecoder) qDebug
#elif DECODERDAV1D_DEBUG_OUTPUT == 3
#define DEBUG_DAV1D if (isCachingDecoder) qDebug("c:"); else qDebug("i:"); qDebug
#endif
#else
#define DEBUG_DAV1D(fmt,...) ((void)0)
#endif

decoderDav1d_Functions::decoderDav1d_Functions() 
{ 
  memset(this, 0, sizeof(*this));
}

decoderDav1d::decoderDav1d(int signalID, bool cachingDecoder) :
  decoderBaseSingleLib(cachingDecoder)
{
  currentOutputBuffer.clear();

  // Libde265 can only decoder HEVC in YUV format
  rawFormat = raw_YUV;

  QSettings settings;
  settings.beginGroup("Decoders");
  loadDecoderLibrary(settings.value("libDav1dFile", "").toString());
  settings.endGroup();

  bool resetDecoder;
  setDecodeSignal(signalID, resetDecoder);
  allocateNewDecoder();
}

decoderDav1d::~decoderDav1d()
{
  if (decoder != nullptr)
  {
    // Free the decoder
    dav1d_close(&decoder);
    if (decoder != nullptr)
      DEBUG_DAV1D("Error closing the decoder. The close function should set the decoder pointer to NULL");
  }
}

void decoderDav1d::resetDecoder()
{
  // Delete decoder
  if (!decoder)
    return setError("Resetting the decoder failed. No decoder allocated.");

  dav1d_flush(decoder);

  decoderBase::resetDecoder();
  currentOutputBuffer.clear();
  decodedFrameWaiting = false;
  flushing = false;
}

void decoderDav1d::setDecodeSignal(int signalID, bool &decoderResetNeeded)
{
  decoderResetNeeded = false;
  if (signalID == decodeSignal)
    return;
  if (signalID >= 0 && signalID < nrSignalsSupported())
    decodeSignal = signalID;
  decoderResetNeeded = true;
}

void decoderDav1d::resolveLibraryFunctionPointers()
{
  // Get/check function pointers
  if (!resolve(dav1d_version, "dav1d_version")) return;
  if (!resolve(dav1d_default_settings, "dav1d_default_settings")) return;
  if (!resolve(dav1d_open, "dav1d_open")) return;
  if (!resolve(dav1d_parse_sequence_header, "dav1d_parse_sequence_header")) return;
  if (!resolve(dav1d_send_data, "dav1d_send_data")) return;
  if (!resolve(dav1d_get_picture, "dav1d_get_picture")) return;
  if (!resolve(dav1d_close, "dav1d_close")) return;
  if (!resolve(dav1d_flush, "dav1d_flush")) return;

  if (!resolve(dav1d_data_create, "dav1d_data_create")) return;

  DEBUG_DAV1D("decoderDav1d::resolveLibraryFunctionPointers - decoding functions found");
}

template <typename T> T decoderDav1d::resolve(T &fun, const char *symbol, bool optional)
{
  QFunctionPointer ptr = library.resolve(symbol);
  if (!ptr)
  {
    if (!optional)
      setError(QStringLiteral("Error loading the libde265 library: Can't find function %1.").arg(symbol));
    return nullptr;
  }

  return fun = reinterpret_cast<T>(ptr);
}

void decoderDav1d::allocateNewDecoder()
{
  if (decoder != nullptr)
  {
    DEBUG_DAV1D("decoderDav1d::allocateNewDecoder Error a decoder was already allocated");
    return;
  }
  if (decoderState == decoderError)
    return;

  DEBUG_DAV1D("decoderDav1d::allocateNewDecoder - decodeSignal %d", decodeSignal);

  dav1d_default_settings(&settings);

  // Create new decoder object
  int err = dav1d_open(&decoder, &settings);
  if (err != 0)
  {
    decoderState = decoderError;
    setError("Error opening new decoder (dav1d_open)");
    return;
  }

  // The decoder is ready to receive data
  decoderBase::resetDecoder();
  currentOutputBuffer.clear();
  decodedFrameWaiting = false;
  flushing = false;
}

bool decoderDav1d::decodeNextFrame()
{
  if (decoderState != decoderRetrieveFrames)
  {
    DEBUG_DAV1D("decoderLibde265::decodeNextFrame: Wrong decoder state.");
    return false;
  }
  if (decodedFrameWaiting)
  {
    decodedFrameWaiting = false;
    return true;
  }

  return decodeFrame();
}

bool decoderDav1d::decodeFrame()
{
  //int more = 1;
  //curImage = nullptr;
  //while (more && curImage == nullptr)
  //{
  //  more = 0;
  //  de265_error err = de265_decode(decoder, &more);

  //  if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA)
  //  {
  //    decoderState = decoderNeedsMoreData;
  //    return false;
  //  }
  //  else if (err != DE265_OK)
  //    return setErrorB("Error decoding (de265_decode)");

  //  curImage = de265_get_next_picture(decoder);
  //}

  //if (more == 0 && curImage == nullptr)
  //{
  //  // Decoding ended
  //  decoderState = decoderEndOfBitstream;
  //  return false;
  //}

  //if (curImage != nullptr)
  //{
  //  // Get the resolution / yuv format from the frame
  //  QSize s = QSize(de265_get_image_width(curImage, 0), de265_get_image_height(curImage, 0));
  //  if (!s.isValid())
  //    DEBUG_DAV1D("decoderLibde265::decodeFrame got invalid frame size");
  //  auto subsampling = convertFromInternalSubsampling(de265_get_chroma_format(curImage));
  //  if (subsampling == YUV_NUM_SUBSAMPLINGS)
  //    DEBUG_DAV1D("decoderLibde265::decodeFrame got invalid subsampling");
  //  int bitDepth = de265_get_bits_per_pixel(curImage, 0);
  //  if (bitDepth < 8 || bitDepth > 16)
  //    DEBUG_DAV1D("decoderLibde265::decodeFrame got invalid bit depth");

  //  if (!frameSize.isValid() && !formatYUV.isValid())
  //  {
  //    // Set the values
  //    frameSize = s;
  //    formatYUV = yuvPixelFormat(subsampling, bitDepth);
  //  }
  //  else
  //  {
  //    // Check the values against the previously set values
  //    if (frameSize != s)
  //      return setErrorB("Recieved a frame of different size");
  //    if (formatYUV.subsampling != subsampling)
  //      return setErrorB("Recieved a frame with different subsampling");
  //    if (formatYUV.bitsPerSample != bitDepth)
  //      return setErrorB("Recieved a frame with different bit depth");
  //  }
  //  DEBUG_DAV1D("decoderLibde265::decodeFrame Picture decoded");

  //  decoderState = decoderRetrieveFrames;
  //  currentOutputBuffer.clear();
  //  return true;
  //}
  return false;
}

QByteArray decoderDav1d::getRawFrameData()
{
  return QByteArray();
  //if (curImage == nullptr)
  //  return QByteArray();
  //if (decoderState != decoderRetrieveFrames)
  //{
  //  DEBUG_DAV1D("decoderLibde265::getRawFrameData: Wrong decoder state.");
  //  return QByteArray();
  //}

  //if (currentOutputBuffer.isEmpty())
  //{
  //  // Put image data into buffer
  //  copyImgToByteArray(curImage, currentOutputBuffer);
  //  DEBUG_DAV1D("decoderLibde265::getRawFrameData copied frame to buffer");

  //  if (retrieveStatistics)
  //    // Get the statistics from the image and put them into the statistics cache
  //    cacheStatistics(curImage);
  //}

  //return currentOutputBuffer;
}

bool decoderDav1d::pushData(QByteArray &data) 
{
  if (decoderState != decoderNeedsMoreData)
  {
    DEBUG_DAV1D("decoderDav1d::pushData: Wrong decoder state.");
    return false;
  }
  if (flushing)
  {
    DEBUG_DAV1D("decoderDav1d::pushData: Do not push data when flushing!");
    return false;
  }

  if (!sequenceHeaderPushed)
  {
    // The first packet which is pushed to the decoder should be a sequence header.
    // Otherwise, the decoder can not decode the data.
    if (data.size() == 0)
    {
      DEBUG_DAV1D("decoderDav1d::pushData Error: Sequence header not pushed yet and the data is empty");
      return setErrorB("Error: Sequence header not pushed yet and the data is empty.");
    }

    Dav1dSequenceHeader seq;
    int err = dav1d_parse_sequence_header(&seq, (const uint8_t*)data.data(), data.size());
    if (err == 0)
      sequenceHeaderPushed = true;
    else
    {
      DEBUG_DAV1D("decoderDav1d::pushData Error: No sequence header revieved yet and parsing of this packet as slice header failed. Ignoring packet.");
      return true;
    }
  }
  else if (data.size() == 0)
  {
    // The input file is at the end. Switch to flushing mode.
    DEBUG_DAV1D("decoderDav1d::pushData input ended - flushing");
    flushing = true;
  }
  else
  {
    // Since dav1d consumes the data (takes ownership), we need to copy it to a new buffer from dav1d
    Dav1dData *dav1dData = new Dav1dData;
    uint8_t *rawDataPointer = dav1d_data_create(dav1dData, data.size());
    memcpy(rawDataPointer, data.data(), data.size());
    
    int err = dav1d_send_data(decoder, dav1dData);
    if (err == -EAGAIN)
    {
      // The data was not consumed and must be pushed again after retrieving some frames
      delete dav1dData;
      return false;
    }
    else if (err != 0)
    {
      delete dav1dData;
      return setErrorB("Error pushing data to the decoder.");
    }
  }

  // Check for an available frame
  if (decodeFrame())
    decodedFrameWaiting = true;

  return true;
}

#if SSE_CONVERSION
void decoderDav1d::copyImgToByteArray(const Dav1dPicture *src, byteArrayAligned &dst)
#else
void decoderDav1d::copyImgToByteArray(const Dav1dPicture *src, QByteArray &dst)
#endif
{
  //// How many image planes are there?
  //de265_chroma cMode = de265_get_chroma_format(src);
  //int nrPlanes = (cMode == de265_chroma_mono) ? 1 : 3;

  //// At first get how many bytes we are going to write
  //int nrBytes = 0;
  //int stride;
  //for (int c = 0; c < nrPlanes; c++)
  //{
  //  int width = de265_get_image_width(src, c);
  //  int height = de265_get_image_height(src, c);
  //  int nrBytesPerSample = (de265_get_bits_per_pixel(src, c) > 8) ? 2 : 1;

  //  nrBytes += width * height * nrBytesPerSample;
  //}

  //DEBUG_DAV1D("decoderLibde265::copyImgToByteArray nrBytes %d", nrBytes);

  //// Is the output big enough?
  //if (dst.capacity() < nrBytes)
  //  dst.resize(nrBytes);

  //uint8_t *dst_c = (uint8_t*)dst.data();

  //// We can now copy from src to dst
  //for (int c = 0; c < nrPlanes; c++)
  //{
  //  const int width = de265_get_image_width(src, c);
  //  const int height = de265_get_image_height(src, c);
  //  const int nrBytesPerSample = (de265_get_bits_per_pixel(src, c) > 8) ? 2 : 1;
  //  const size_t widthInBytes = width * nrBytesPerSample;

  //  const uint8_t* img_c = nullptr;
  //  if (decodeSignal == 0)
  //    img_c = de265_get_image_plane(src, c, &stride);
  //  else if (decodeSignal == 1)
  //    img_c = de265_internals_get_image_plane(src, DE265_INTERNALS_DECODER_PARAM_SAVE_PREDICTION, c, &stride);
  //  else if (decodeSignal == 2)
  //    img_c = de265_internals_get_image_plane(src, DE265_INTERNALS_DECODER_PARAM_SAVE_RESIDUAL, c, &stride);
  //  else if (decodeSignal == 3)
  //    img_c = de265_internals_get_image_plane(src, DE265_INTERNALS_DECODER_PARAM_SAVE_TR_COEFF, c, &stride);

  //  if (img_c == nullptr)
  //    return;

  //  for (int y = 0; y < height; y++)
  //  {
  //    memcpy(dst_c, img_c, widthInBytes);
  //    img_c += stride;
  //    dst_c += widthInBytes;
  //  }
  //}
}

bool decoderDav1d::checkLibraryFile(QString libFilePath, QString &error)
{
  decoderDav1d testDecoder;

  // Try to load the library file
  testDecoder.library.setFileName(libFilePath);
  if (!testDecoder.library.load())
  {
    error = "Error opening QLibrary.";
    return false;
  }

  // Now let's see if we can retrive all the function pointers that we will need.
  // If this works, we can be fairly certain that this is a valid libde265 library.
  testDecoder.resolveLibraryFunctionPointers();
  error = testDecoder.decoderErrorString();
  return !testDecoder.errorInDecoder();
}

QString decoderDav1d::getDecoderName() const
{
  if (decoder)
  {
    QString ver = QString(dav1d_version());
    return "Dav1d deoder Version " + ver;
  }
  return "Dav1d decoder";
}

QStringList decoderDav1d::getLibraryNames()
{
  // If the file name is not set explicitly, QLibrary will try to open
  // the libde265.so file first. Since this has been compiled for linux
  // it will fail and not even try to open the libde265.dylib.
  // On windows and linux ommitting the extension works
  if (is_Q_OS_MAC)
    return QStringList() << "libdav1d-internals.dylib" << "libdav1d.dylib";
  if (is_Q_OS_WIN)
    return QStringList() << "dav1d-internals" << "dav1d";
  if (is_Q_OS_LINUX)
    return QStringList() << "libdav1d-internals" << "libdav1d";
}
