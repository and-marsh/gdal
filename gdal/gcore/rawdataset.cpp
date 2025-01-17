/******************************************************************************
 *
 * Project:  Generic Raw Binary Driver
 * Purpose:  Implementation of RawDataset and RawRasterBand classes.
 * Author:   Frank Warmerdam, warmerda@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "rawdataset.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#if HAVE_FCNTL_H
#  include <fcntl.h>
#endif
#include <algorithm>
#include <limits>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_virtualmem.h"
#include "cpl_vsi.h"
#include "cpl_safemaths.hpp"
#include "gdal.h"
#include "gdal_priv.h"

CPL_CVSID("$Id$")

/************************************************************************/
/*                           RawRasterBand()                            */
/************************************************************************/

RawRasterBand::RawRasterBand( GDALDataset *poDSIn, int nBandIn,
                              VSILFILE *fpRawLIn, vsi_l_offset nImgOffsetIn,
                              int nPixelOffsetIn, int nLineOffsetIn,
                              GDALDataType eDataTypeIn, int bNativeOrderIn,
                              OwnFP bOwnsFPIn ) :
    fpRawL(fpRawLIn),
    nImgOffset(nImgOffsetIn),
    nPixelOffset(nPixelOffsetIn),
    nLineOffset(nLineOffsetIn),
    bNativeOrder(bNativeOrderIn),
    bOwnsFP(bOwnsFPIn == OwnFP::YES)
{
    poDS = poDSIn;
    nBand = nBandIn;
    eDataType = eDataTypeIn;
    nRasterXSize = poDSIn->GetRasterXSize();
    nRasterYSize = poDSIn->GetRasterYSize();

    CPLDebug("GDALRaw",
             "RawRasterBand(%p,%d,%p,\n"
             "              Off=%d,PixOff=%d,LineOff=%d,%s,%d)",
             poDS, nBand, fpRawL,
             static_cast<unsigned int>(nImgOffset), nPixelOffset, nLineOffset,
             GDALGetDataTypeName(eDataType), bNativeOrder);

    // Treat one scanline as the block size.
    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;

    // Initialize other fields, and setup the line buffer.
    Initialize();
}

/************************************************************************/
/*                           RawRasterBand()                            */
/************************************************************************/

RawRasterBand::RawRasterBand( VSILFILE *fpRawLIn, vsi_l_offset nImgOffsetIn,
                              int nPixelOffsetIn, int nLineOffsetIn,
                              GDALDataType eDataTypeIn, int bNativeOrderIn,
                              int nXSize, int nYSize,
                              OwnFP bOwnsFPIn ) :
    fpRawL(fpRawLIn),
    nImgOffset(nImgOffsetIn),
    nPixelOffset(nPixelOffsetIn),
    nLineOffset(nLineOffsetIn),
    nLineSize(0),
    bNativeOrder(bNativeOrderIn),
    nLoadedScanline(0),
    pLineStart(nullptr),
    bDirty(FALSE),
    poCT(nullptr),
    eInterp(GCI_Undefined),
    papszCategoryNames(nullptr),
    bOwnsFP(bOwnsFPIn == OwnFP::YES)
{
    poDS = nullptr;
    nBand = 1;
    eDataType = eDataTypeIn;

    CPLDebug("GDALRaw",
             "RawRasterBand(floating,Off=%d,PixOff=%d,LineOff=%d,%s,%d)",
             static_cast<unsigned int>(nImgOffset),
             nPixelOffset, nLineOffset,
             GDALGetDataTypeName(eDataType), bNativeOrder);

    // Treat one scanline as the block size.
    nBlockXSize = nXSize;
    nBlockYSize = 1;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    if (!GDALCheckDatasetDimensions(nXSize, nYSize))
    {
        pLineBuffer = nullptr;
        return;
    }

    // Initialize other fields, and setup the line buffer.
    Initialize();
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

void RawRasterBand::Initialize()

{
    poCT = nullptr;
    eInterp = GCI_Undefined;

    papszCategoryNames = nullptr;

    bDirty = FALSE;

    vsi_l_offset nSmallestOffset = nImgOffset;
    vsi_l_offset nLargestOffset = nImgOffset;
    if( nLineOffset < 0 )
    {
        if( static_cast<vsi_l_offset>(-nLineOffset) * (nRasterYSize - 1) > nImgOffset )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Inconsistent nLineOffset, nRasterYSize and nImgOffset");
            pLineBuffer = nullptr;
            return;
        }
        nSmallestOffset -= static_cast<vsi_l_offset>(-nLineOffset) * (nRasterYSize - 1);
    }
    else
    {
        if( nImgOffset > std::numeric_limits<vsi_l_offset>::max() -
                    static_cast<vsi_l_offset>(nLineOffset) * (nRasterYSize - 1) )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Inconsistent nLineOffset, nRasterYSize and nImgOffset");
            pLineBuffer = nullptr;
            return;
        }
        nLargestOffset += static_cast<vsi_l_offset>(nLineOffset) * (nRasterYSize - 1);
    }
    if( nPixelOffset < 0 )
    {
        if( static_cast<vsi_l_offset>(-nPixelOffset) * (nRasterXSize - 1) > nSmallestOffset )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Inconsistent nPixelOffset, nRasterXSize and nImgOffset");
            pLineBuffer = nullptr;
            return;
        }
    }
    else
    {
        if( nLargestOffset > std::numeric_limits<vsi_l_offset>::max() -
                    static_cast<vsi_l_offset>(nPixelOffset) * (nRasterXSize - 1) )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Inconsistent nPixelOffset, nRasterXSize and nImgOffset");
            pLineBuffer = nullptr;
            return;
        }
        nLargestOffset += static_cast<vsi_l_offset>(nPixelOffset) * (nRasterXSize - 1);
    }
    if( nLargestOffset > static_cast<vsi_l_offset>(GINTBIG_MAX) )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too big largest offset");
        pLineBuffer = nullptr;
        return;
    }

    // Allocate working scanline.
    nLoadedScanline = -1;
    const int nDTSize = GDALGetDataTypeSizeBytes(GetRasterDataType());
    if (nBlockXSize <= 0 ||
        (nBlockXSize > 1 && std::abs(nPixelOffset) >
            std::numeric_limits<int>::max() / (nBlockXSize - 1)) ||
        std::abs(nPixelOffset) * (nBlockXSize - 1) >
            std::numeric_limits<int>::max() - nDTSize)
    {
        nLineSize = 0;
        pLineBuffer = nullptr;
    }
    else
    {
        nLineSize = std::abs(nPixelOffset) * (nBlockXSize - 1) + nDTSize;
        pLineBuffer = VSIMalloc(nLineSize);
    }
    if (pLineBuffer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Could not allocate line buffer: "
                 "nPixelOffset=%d, nBlockXSize=%d",
                 nPixelOffset, nBlockXSize);
    }

    if( nPixelOffset >= 0 )
        pLineStart = pLineBuffer;
    else
        pLineStart = static_cast<char *>(pLineBuffer) +
                     static_cast<std::ptrdiff_t>(std::abs(nPixelOffset)) *
                         (nBlockXSize - 1);
}

/************************************************************************/
/*                           ~RawRasterBand()                           */
/************************************************************************/

RawRasterBand::~RawRasterBand()

{
    if( poCT )
        delete poCT;

    CSLDestroy(papszCategoryNames);

    RawRasterBand::FlushCache();

    if (bOwnsFP)
    {
        if( VSIFCloseL(fpRawL) != 0 )
        {
            CPLError(CE_Failure, CPLE_FileIO, "I/O error");
        }
    }

    CPLFree(pLineBuffer);
}

/************************************************************************/
/*                             SetAccess()                              */
/************************************************************************/

void RawRasterBand::SetAccess(GDALAccess eAccessIn) { eAccess = eAccessIn; }

/************************************************************************/
/*                             FlushCache()                             */
/*                                                                      */
/*      We override this so we have the opportunity to call             */
/*      fflush().  We don't want to do this all the time in the         */
/*      write block function as it is kind of expensive.                */
/************************************************************************/

CPLErr RawRasterBand::FlushCache()

{
    CPLErr eErr = GDALRasterBand::FlushCache();
    if( eErr != CE_None )
    {
        bDirty = FALSE;
        return eErr;
    }

    // If we have unflushed raw, flush it to disk now.
    if ( bDirty )
    {
        int nRet = VSIFFlushL(fpRawL);

        bDirty = FALSE;
        if( nRet < 0 )
            return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                             AccessLine()                             */
/************************************************************************/

CPLErr RawRasterBand::AccessLine( int iLine )

{
    if (pLineBuffer == nullptr)
        return CE_Failure;

    if( nLoadedScanline == iLine )
        return CE_None;

    // Figure out where to start reading.
    // Write formulas such that unsigned int overflow doesn't occur
    vsi_l_offset nReadStart = nImgOffset;
    if( nLineOffset >= 0 )
    {
        nReadStart += static_cast<GUIntBig>(nLineOffset) * iLine;
    }
    else
    {
        nReadStart -= static_cast<GUIntBig>(-static_cast<GIntBig>(nLineOffset)) * iLine;
    }
    if( nPixelOffset < 0 )
    {
        const GUIntBig nPixelOffsetToSubtract =
            static_cast<GUIntBig>(-static_cast<GIntBig>(nPixelOffset)) * (nBlockXSize - 1);
        nReadStart -= nPixelOffsetToSubtract;
    }

    // Seek to the correct line.
    if( Seek(nReadStart, SEEK_SET) == -1 )
    {
        if (poDS != nullptr && poDS->GetAccess() == GA_ReadOnly)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Failed to seek to scanline %d @ " CPL_FRMT_GUIB ".",
                     iLine, nReadStart);
            return CE_Failure;
        }
        else
        {
            memset(pLineBuffer, 0, nLineSize);
            nLoadedScanline = iLine;
            return CE_None;
        }
    }

    // Read the line.  Take care not to request any more bytes than
    // are needed, and not to lose a partially successful scanline read.
    const size_t nBytesToRead = nLineSize;
    const size_t nBytesActuallyRead = Read(pLineBuffer, 1, nBytesToRead);
    if( nBytesActuallyRead < nBytesToRead )
    {
        if (poDS != nullptr && poDS->GetAccess() == GA_ReadOnly &&
            // ENVI datasets might be sparse (see #915)
            poDS->GetMetadata("ENVI") == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Failed to read scanline %d.",
                     iLine);
            return CE_Failure;
        }
        else
        {
            memset(
                static_cast<GByte *>(pLineBuffer) + nBytesActuallyRead,
                0, nBytesToRead - nBytesActuallyRead);
        }
    }

    // Byte swap the interesting data, if required.
    if( !bNativeOrder && eDataType != GDT_Byte )
    {
        if( GDALDataTypeIsComplex(eDataType) )
        {
            const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
            GDALSwapWords(pLineBuffer, nWordSize, nBlockXSize,
                          std::abs(nPixelOffset));
            GDALSwapWords(
                static_cast<GByte *>(pLineBuffer) + nWordSize,
                nWordSize, nBlockXSize, std::abs(nPixelOffset));
        }
        else
        {
            GDALSwapWords(pLineBuffer, GDALGetDataTypeSizeBytes(eDataType),
                          nBlockXSize, std::abs(nPixelOffset));
        }
    }

    nLoadedScanline = iLine;

    return CE_None;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr RawRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff,
                                 int nBlockYOff,
                                 void *pImage)
{
    CPLAssert(nBlockXOff == 0);

    if (pLineBuffer == nullptr)
        return CE_Failure;

    const CPLErr eErr = AccessLine(nBlockYOff);
    if( eErr == CE_Failure )
        return eErr;

    // Copy data from disk buffer to user block buffer.
    GDALCopyWords(pLineStart, eDataType, nPixelOffset,
                  pImage, eDataType, GDALGetDataTypeSizeBytes(eDataType),
                  nBlockXSize);

    return eErr;
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr RawRasterBand::IWriteBlock( CPL_UNUSED int nBlockXOff,
                                   int nBlockYOff,
                                   void *pImage )
{
    CPLAssert(nBlockXOff == 0);

    if (pLineBuffer == nullptr)
        return CE_Failure;

    // If the data for this band is completely contiguous, we don't
    // have to worry about pre-reading from disk.
    CPLErr eErr = CE_None;
    if( std::abs(nPixelOffset) > GDALGetDataTypeSizeBytes(eDataType) )
        eErr = AccessLine(nBlockYOff);

    // Copy data from user buffer into disk buffer.
    GDALCopyWords(pImage, eDataType, GDALGetDataTypeSizeBytes(eDataType),
                  pLineStart, eDataType, nPixelOffset, nBlockXSize);


    // Byte swap (if necessary) back into disk order before writing.
    if( !bNativeOrder && eDataType != GDT_Byte )
    {
        if( GDALDataTypeIsComplex(eDataType) )
        {
            const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
            GDALSwapWords(pLineBuffer, nWordSize, nBlockXSize,
                          std::abs(nPixelOffset));
            GDALSwapWords(static_cast<GByte *>(pLineBuffer) + nWordSize,
                          nWordSize, nBlockXSize, std::abs(nPixelOffset));
        }
        else
        {
            GDALSwapWords(pLineBuffer, GDALGetDataTypeSizeBytes(eDataType),
                          nBlockXSize, std::abs(nPixelOffset));
        }
    }

    // Figure out where to start writing.
    // Write formulas such that unsigned int overflow doesn't occur
    const GUIntBig nPixelOffsetToSubtract =
        nPixelOffset >= 0
        ? 0 : static_cast<GUIntBig>(-static_cast<GIntBig>(nPixelOffset)) * (nBlockXSize - 1);
    const vsi_l_offset nWriteStart = static_cast<vsi_l_offset>(
        (nLineOffset >= 0 ?
            nImgOffset + static_cast<GUIntBig>(nLineOffset) * nBlockYOff :
            nImgOffset - static_cast<GUIntBig>(-static_cast<GIntBig>(nLineOffset)) * nBlockYOff )
        - nPixelOffsetToSubtract);

    // Seek to correct location.
    if( Seek(nWriteStart, SEEK_SET) == -1 )
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Failed to seek to scanline %d @ " CPL_FRMT_GUIB
                 " to write to file.",
                 nBlockYOff, nImgOffset + nBlockYOff * nLineOffset);

        eErr = CE_Failure;
    }

    // Write data buffer.
    const int nBytesToWrite = nLineSize;
    if( eErr == CE_None
        && Write(pLineBuffer, 1, nBytesToWrite)
        < static_cast<size_t>(nBytesToWrite) )
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Failed to write scanline %d to file.",
                 nBlockYOff);

        eErr = CE_Failure;
    }

    // Byte swap (if necessary) back into machine order so the
    // buffer is still usable for reading purposes.
    if( !bNativeOrder && eDataType != GDT_Byte )
    {
        if( GDALDataTypeIsComplex(eDataType) )
        {
            const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
            GDALSwapWords(pLineBuffer, nWordSize, nBlockXSize,
                          std::abs(nPixelOffset));
            GDALSwapWords(static_cast<GByte *>(pLineBuffer) +
                          nWordSize, nWordSize, nBlockXSize,
                          std::abs(nPixelOffset));
        }
        else
        {
            GDALSwapWords(pLineBuffer, GDALGetDataTypeSizeBytes(eDataType),
                          nBlockXSize, std::abs(nPixelOffset));
        }
    }

    bDirty = TRUE;
    return eErr;
}

/************************************************************************/
/*                             AccessBlock()                            */
/************************************************************************/

CPLErr RawRasterBand::AccessBlock(vsi_l_offset nBlockOff, size_t nBlockSize,
                                  void *pData)
{
    // Seek to the correct block.
    if( Seek(nBlockOff, SEEK_SET) == -1 )
    {
        memset(pData, 0, nBlockSize);
        return CE_None;
    }

    // Read the block.
    const size_t nBytesActuallyRead = Read(pData, 1, nBlockSize);
    if( nBytesActuallyRead < nBlockSize )
    {

        memset(static_cast<GByte *>(pData) + nBytesActuallyRead,
               0, nBlockSize - nBytesActuallyRead);
        return CE_None;
    }

    // Byte swap the interesting data, if required.
    if( !bNativeOrder && eDataType != GDT_Byte )
    {
        if( GDALDataTypeIsComplex(eDataType) )
        {
            const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
            GDALSwapWordsEx(pData, nWordSize, nBlockSize / nPixelOffset,
                            nPixelOffset);
            GDALSwapWordsEx(static_cast<GByte *>(pData) + nWordSize,
                            nWordSize, nBlockSize / nPixelOffset, nPixelOffset);
        }
        else
        {
            GDALSwapWordsEx(pData, GDALGetDataTypeSizeBytes(eDataType),
                            nBlockSize / nPixelOffset, nPixelOffset);
        }
    }

    return CE_None;
}

/************************************************************************/
/*               IsSignificantNumberOfLinesLoaded()                     */
/*                                                                      */
/*  Check if there is a significant number of scanlines (>20%) from the */
/*  specified block of lines already cached.                            */
/************************************************************************/

int RawRasterBand::IsSignificantNumberOfLinesLoaded( int nLineOff, int nLines )
{
    int nCountLoaded = 0;

    for ( int iLine = nLineOff; iLine < nLineOff + nLines; iLine++ )
    {
        GDALRasterBlock *poBlock = TryGetLockedBlockRef(0, iLine);
        if( poBlock != nullptr )
        {
            poBlock->DropLock();
            nCountLoaded++;
            if( nCountLoaded > nLines / 20 )
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/************************************************************************/
/*                           CanUseDirectIO()                           */
/************************************************************************/

int RawRasterBand::CanUseDirectIO(int /* nXOff */,
                                  int nYOff,
                                  int nXSize,
                                  int nYSize,
                                  GDALDataType /* eBufType*/,
                                  GDALRasterIOExtraArg* psExtraArg)
{

    // Use direct IO without caching if:
    //
    // GDAL_ONE_BIG_READ is enabled
    //
    // or
    //
    // the length of a scanline on disk is more than 50000 bytes, and the
    // width of the requested chunk is less than 40% of the whole scanline and
    // no significant number of requested scanlines are already in the cache.

    if( nPixelOffset < 0 ||
        psExtraArg->eResampleAlg != GRIORA_NearestNeighbour )
    {
        return FALSE;
    }

    const char *pszGDAL_ONE_BIG_READ =
        CPLGetConfigOption("GDAL_ONE_BIG_READ", nullptr);
    if ( pszGDAL_ONE_BIG_READ == nullptr )
    {
        if ( nLineSize < 50000
             || nXSize > nLineSize / nPixelOffset / 5 * 2
             || IsSignificantNumberOfLinesLoaded(nYOff, nYSize) )
        {
            return FALSE;
        }
        return TRUE;
    }

    return CPLTestBool(pszGDAL_ONE_BIG_READ);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr RawRasterBand::IRasterIO( GDALRWFlag eRWFlag,
                                 int nXOff, int nYOff, int nXSize, int nYSize,
                                 void * pData, int nBufXSize, int nBufYSize,
                                 GDALDataType eBufType,
                                 GSpacing nPixelSpace, GSpacing nLineSpace,
                                 GDALRasterIOExtraArg* psExtraArg )

{
    const int nBandDataSize = GDALGetDataTypeSizeBytes(eDataType);
#ifdef DEBUG
    // Otherwise Coverity thinks that a divide by zero is possible in
    // AccessBlock() in the complex data type wapping case.
    if( nBandDataSize == 0 )
        return CE_Failure;
#endif
    const int nBufDataSize = GDALGetDataTypeSizeBytes(eBufType);

    if( !CanUseDirectIO(nXOff, nYOff, nXSize, nYSize, eBufType, psExtraArg) )
    {
        return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff,
                                         nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize,
                                         eBufType,
                                         nPixelSpace, nLineSpace, psExtraArg);
    }

    CPLDebug("RAW", "Using direct IO implementation");

    // Read data.
    if ( eRWFlag == GF_Read )
    {
        // Do we have overviews that are appropriate to satisfy this request?
        if( (nBufXSize < nXSize || nBufYSize < nYSize)
            && GetOverviewCount() > 0 )
        {
            if( OverviewRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                 pData, nBufXSize, nBufYSize,
                                 eBufType, nPixelSpace, nLineSpace,
                                 psExtraArg) == CE_None)
                return CE_None;
        }

        // 1. Simplest case when we should get contiguous block
        //    of uninterleaved pixels.
        if ( nXSize == GetXSize()
             && nXSize == nBufXSize
             && nYSize == nBufYSize
             && eBufType == eDataType
             && nPixelOffset == nBandDataSize
             && nPixelSpace == nBufDataSize
             && nLineSpace == nPixelSpace * nXSize )
        {
            vsi_l_offset nOffset = nImgOffset;
            if( nLineOffset >= 0 )
                nOffset += nYOff * nLineOffset;
            else
                nOffset -= nYOff * static_cast<vsi_l_offset>(-nLineOffset);

            const size_t nBytesToRead =
                static_cast<size_t>(nXSize) * nYSize * nBandDataSize;
            if ( AccessBlock(nOffset, nBytesToRead, pData) != CE_None )
            {
                CPLError(CE_Failure, CPLE_FileIO,
                         "Failed to read " CPL_FRMT_GUIB
                         " bytes at " CPL_FRMT_GUIB ".",
                         static_cast<GUIntBig>(nBytesToRead), nOffset);
                return CE_Failure;
            }
        }
        // 2. Case when we need deinterleave and/or subsample data.
        else
        {
            const double dfSrcXInc = static_cast<double>(nXSize) / nBufXSize;
            const double dfSrcYInc = static_cast<double>(nYSize) / nBufYSize;

            const size_t nBytesToRW =
                static_cast<size_t>(nPixelOffset) * nXSize;
            GByte *pabyData =
                static_cast<GByte *>(VSI_MALLOC_VERBOSE(nBytesToRW));
            if( pabyData == nullptr )
                return CE_Failure;

            for ( int iLine = 0; iLine < nBufYSize; iLine++ )
            {
                const vsi_l_offset nLine =
                    static_cast<vsi_l_offset>(nYOff) +
                      static_cast<vsi_l_offset>(iLine * dfSrcYInc);
                vsi_l_offset nOffset = nImgOffset;
                if( nLineOffset >= 0 )
                    nOffset += nLine * nLineOffset;
                else
                    nOffset -= nLine * static_cast<vsi_l_offset>(-nLineOffset);
                if( nPixelOffset >= 0 )
                    nOffset += nXOff * nPixelOffset;
                else
                    nOffset -= nXOff * static_cast<vsi_l_offset>(-nPixelOffset);
                if ( AccessBlock(nOffset,
                                 nBytesToRW, pabyData) != CE_None )
                {
                    CPLError(CE_Failure, CPLE_FileIO,
                             "Failed to read " CPL_FRMT_GUIB
                             " bytes at " CPL_FRMT_GUIB ".",
                             static_cast<GUIntBig>(nBytesToRW), nOffset);
                    CPLFree(pabyData);
                    return CE_Failure;
                }
                // Copy data from disk buffer to user block buffer and
                // subsample, if needed.
                if ( nXSize == nBufXSize && nYSize == nBufYSize )
                {
                    GDALCopyWords(
                        pabyData, eDataType, nPixelOffset,
                        static_cast<GByte *>(pData) +
                            static_cast<vsi_l_offset>(iLine) * nLineSpace,
                        eBufType, static_cast<int>(nPixelSpace), nXSize);
                }
                else
                {
                    for ( int iPixel = 0; iPixel < nBufXSize; iPixel++ )
                    {
                        GDALCopyWords(
                            pabyData +
                                static_cast<vsi_l_offset>(iPixel * dfSrcXInc) *
                                    nPixelOffset,
                            eDataType, nPixelOffset,
                            static_cast<GByte *>(pData) +
                                static_cast<vsi_l_offset>(iLine) * nLineSpace +
                                static_cast<vsi_l_offset>(iPixel) * nPixelSpace,
                            eBufType, static_cast<int>(nPixelSpace), 1);
                    }
                }

                if( psExtraArg->pfnProgress != nullptr &&
                    !psExtraArg->pfnProgress(1.0 * (iLine + 1) / nBufYSize, "",
                                            psExtraArg->pProgressData) )
                {
                    CPLFree(pabyData);
                    return CE_Failure;
                }
            }

            CPLFree(pabyData);
        }
    }
    // Write data.
    else
    {
        // 1. Simplest case when we should write contiguous block of
        //    uninterleaved pixels.
        if ( nXSize == GetXSize()
             && nXSize == nBufXSize
             && nYSize == nBufYSize
             && eBufType == eDataType
             && nPixelOffset == nBandDataSize
             && nPixelSpace == nBufDataSize
             && nLineSpace == nPixelSpace * nXSize )
        {
            // Byte swap the data buffer, if required.
            if( !bNativeOrder && eDataType != GDT_Byte )
            {
                if( GDALDataTypeIsComplex(eDataType) )
                {
                    const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
                    GDALSwapWords(pData, nWordSize, nXSize, nPixelOffset);
                    GDALSwapWords(static_cast<GByte *>(pData) + nWordSize,
                                  nWordSize, nXSize, nPixelOffset);
                }
                else
                {
                    GDALSwapWords(pData, nBandDataSize, nXSize, nPixelOffset);
                }
            }

            // Seek to the correct block.
            vsi_l_offset nOffset = nImgOffset;
            if( nLineOffset >= 0 )
                nOffset += nYOff * nLineOffset;
            else
                nOffset -= nYOff * static_cast<vsi_l_offset>(-nLineOffset);

            if( Seek(nOffset, SEEK_SET) == -1 )
            {
                CPLError(CE_Failure, CPLE_FileIO,
                         "Failed to seek to " CPL_FRMT_GUIB " to write data.",
                         nOffset);

                return CE_Failure;
            }

            // Write the block.
            const size_t nBytesToRW =
                static_cast<size_t>(nXSize) * nYSize * nBandDataSize;

            const size_t nBytesActuallyWritten = Write(pData, 1, nBytesToRW);
            if( nBytesActuallyWritten < nBytesToRW )
            {
                CPLError(CE_Failure, CPLE_FileIO,
                         "Failed to write " CPL_FRMT_GUIB
                         " bytes to file. " CPL_FRMT_GUIB " bytes written",
                         static_cast<GUIntBig>(nBytesToRW),
                         static_cast<GUIntBig>(nBytesActuallyWritten));

                return CE_Failure;
            }

            // Byte swap (if necessary) back into machine order so the
            // buffer is still usable for reading purposes.
            if( !bNativeOrder  && eDataType != GDT_Byte )
            {
                if( GDALDataTypeIsComplex(eDataType) )
                {
                    const int nWordSize = GDALGetDataTypeSize(eDataType) / 16;
                    GDALSwapWords(pData, nWordSize, nXSize, nPixelOffset);
                    GDALSwapWords(static_cast<GByte *>(pData) + nWordSize,
                                  nWordSize, nXSize, nPixelOffset);
                }
                else
                {
                    GDALSwapWords(pData, nBandDataSize, nXSize, nPixelOffset);
                }
            }
        }
        // 2. Case when we need deinterleave and/or subsample data.
        else
        {
            const double dfSrcXInc = static_cast<double>(nXSize) / nBufXSize;
            const double dfSrcYInc = static_cast<double>(nYSize) / nBufYSize;

            const size_t nBytesToRW =
                static_cast<size_t>(nPixelOffset) * nXSize;
            GByte *pabyData =
                static_cast<GByte *>(VSI_MALLOC_VERBOSE(nBytesToRW));
            if( pabyData == nullptr )
                return CE_Failure;

            for ( int iLine = 0; iLine < nBufYSize; iLine++ )
            {
                const vsi_l_offset nLine =
                    static_cast<vsi_l_offset>(nYOff) +
                      static_cast<vsi_l_offset>(iLine * dfSrcYInc);
                vsi_l_offset nOffset = nImgOffset;
                if( nLineOffset >= 0 )
                    nOffset += nLine * nLineOffset;
                else
                    nOffset -= nLine * static_cast<vsi_l_offset>(-nLineOffset);
                if( nPixelOffset >= 0 )
                    nOffset += nXOff * nPixelOffset;
                else
                    nOffset -= nXOff * static_cast<vsi_l_offset>(-nPixelOffset);

                // If the data for this band is completely contiguous we don't
                // have to worry about pre-reading from disk.
                if( nPixelOffset > nBandDataSize )
                    AccessBlock(nOffset, nBytesToRW, pabyData);

                // Copy data from user block buffer to disk buffer and
                // subsample, if needed.
                if ( nXSize == nBufXSize && nYSize == nBufYSize )
                {
                    GDALCopyWords(static_cast<GByte *>(pData) +
                                      static_cast<vsi_l_offset>(iLine) *
                                          nLineSpace,
                                  eBufType, static_cast<int>(nPixelSpace),
                                  pabyData, eDataType, nPixelOffset, nXSize);
                }
                else
                {
                    for ( int iPixel = 0; iPixel < nBufXSize; iPixel++ )
                    {
                        GDALCopyWords(
                            static_cast<GByte *>(pData) +
                                static_cast<vsi_l_offset>(iLine) * nLineSpace +
                                static_cast<vsi_l_offset>(iPixel) * nPixelSpace,
                            eBufType, static_cast<int>(nPixelSpace),
                            pabyData +
                                static_cast<vsi_l_offset>(iPixel * dfSrcXInc) *
                                    nPixelOffset,
                            eDataType, nPixelOffset, 1);
                    }
                }

                // Byte swap the data buffer, if required.
                if( !bNativeOrder && eDataType != GDT_Byte )
                {
                    if( GDALDataTypeIsComplex(eDataType) )
                    {
                        const int nWordSize =
                            GDALGetDataTypeSize(eDataType) / 16;
                        GDALSwapWords(pabyData, nWordSize, nXSize,
                                      nPixelOffset);
                        GDALSwapWords(static_cast<GByte *>(pabyData) +
                                          nWordSize,
                                      nWordSize, nXSize, nPixelOffset);
                    }
                    else
                    {
                        GDALSwapWords(pabyData, nBandDataSize, nXSize,
                                      nPixelOffset);
                    }
                }

                // Seek to the right line in block.
                if( Seek(nOffset, SEEK_SET) == -1 )
                {
                    CPLError(CE_Failure, CPLE_FileIO,
                             "Failed to seek to " CPL_FRMT_GUIB " to read.",
                             nOffset);
                    CPLFree(pabyData);
                    return CE_Failure;
                }

                // Write the line of block.
                const size_t nBytesActuallyWritten =
                    Write(pabyData, 1, nBytesToRW);
                if( nBytesActuallyWritten < nBytesToRW )
                {
                    CPLError(CE_Failure, CPLE_FileIO,
                             "Failed to write " CPL_FRMT_GUIB
                             " bytes to file. " CPL_FRMT_GUIB " bytes written",
                             static_cast<GUIntBig>(nBytesToRW),
                             static_cast<GUIntBig>(nBytesActuallyWritten));
                    CPLFree(pabyData);
                    return CE_Failure;
                }


                // Byte swap (if necessary) back into machine order so the
                // buffer is still usable for reading purposes.
                if( !bNativeOrder && eDataType != GDT_Byte )
                {
                    if( GDALDataTypeIsComplex(eDataType) )
                    {
                        const int nWordSize =
                            GDALGetDataTypeSize(eDataType) / 16;
                        GDALSwapWords(pabyData, nWordSize, nXSize,
                                      nPixelOffset);
                        GDALSwapWords(static_cast<GByte *>(pabyData) +
                                          nWordSize,
                                      nWordSize, nXSize, nPixelOffset);
                    }
                    else
                    {
                        GDALSwapWords(pabyData, nBandDataSize, nXSize,
                                      nPixelOffset);
                    }
                }
            }

            bDirty = TRUE;
            CPLFree(pabyData);
        }
    }

    return CE_None;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int RawRasterBand::Seek( vsi_l_offset nOffset, int nSeekMode )

{
    return VSIFSeekL(fpRawL, nOffset, nSeekMode);
}

/************************************************************************/
/*                                Read()                                */
/************************************************************************/

size_t RawRasterBand::Read( void *pBuffer, size_t nSize, size_t nCount )

{
    return VSIFReadL(pBuffer, nSize, nCount, fpRawL);
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t RawRasterBand::Write( void *pBuffer, size_t nSize, size_t nCount )

{
    return VSIFWriteL(pBuffer, nSize, nCount, fpRawL);
}

/************************************************************************/
/*                          StoreNoDataValue()                          */
/*                                                                      */
/*      This is a helper function for datasets to associate a no        */
/*      data value with this band, it isn't intended to be called by    */
/*      applications.                                                   */
/************************************************************************/

void RawRasterBand::StoreNoDataValue( double dfValue )

{
    SetNoDataValue(dfValue);
}

/************************************************************************/
/*                          GetCategoryNames()                          */
/************************************************************************/

char **RawRasterBand::GetCategoryNames() { return papszCategoryNames; }

/************************************************************************/
/*                          SetCategoryNames()                          */
/************************************************************************/

CPLErr RawRasterBand::SetCategoryNames( char **papszNewNames )

{
    CSLDestroy(papszCategoryNames);
    papszCategoryNames = CSLDuplicate(papszNewNames);

    return CE_None;
}

/************************************************************************/
/*                           SetColorTable()                            */
/************************************************************************/

CPLErr RawRasterBand::SetColorTable( GDALColorTable *poNewCT )

{
    if( poCT )
        delete poCT;
    if( poNewCT == nullptr )
        poCT = nullptr;
    else
        poCT = poNewCT->Clone();

    return CE_None;
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *RawRasterBand::GetColorTable() { return poCT; }

/************************************************************************/
/*                       SetColorInterpretation()                       */
/************************************************************************/

CPLErr RawRasterBand::SetColorInterpretation( GDALColorInterp eNewInterp )

{
    eInterp = eNewInterp;

    return CE_None;
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp RawRasterBand::GetColorInterpretation() { return eInterp; }

/************************************************************************/
/*                           GetVirtualMemAuto()                        */
/************************************************************************/

CPLVirtualMem  *RawRasterBand::GetVirtualMemAuto( GDALRWFlag eRWFlag,
                                                  int *pnPixelSpace,
                                                  GIntBig *pnLineSpace,
                                                  char **papszOptions )
{
    CPLAssert(pnPixelSpace);
    CPLAssert(pnLineSpace);

    const vsi_l_offset nSize =
        static_cast<vsi_l_offset>(nRasterYSize - 1) * nLineOffset +
        (nRasterXSize - 1) * nPixelOffset + GDALGetDataTypeSizeBytes(eDataType);

    const char *pszImpl = CSLFetchNameValueDef(
            papszOptions, "USE_DEFAULT_IMPLEMENTATION", "AUTO");
    if( VSIFGetNativeFileDescriptorL(fpRawL) == nullptr ||
        !CPLIsVirtualMemFileMapAvailable() ||
        (eDataType != GDT_Byte && !bNativeOrder) ||
        static_cast<size_t>(nSize) != nSize ||
        nPixelOffset < 0 ||
        nLineOffset < 0 ||
        EQUAL(pszImpl, "YES") || EQUAL(pszImpl, "ON") ||
        EQUAL(pszImpl, "1") || EQUAL(pszImpl, "TRUE") )
    {
        return GDALRasterBand::GetVirtualMemAuto(eRWFlag, pnPixelSpace,
                                                 pnLineSpace, papszOptions);
    }

    FlushCache();

    CPLVirtualMem *pVMem = CPLVirtualMemFileMapNew(
        fpRawL, nImgOffset, nSize,
        (eRWFlag == GF_Write) ? VIRTUALMEM_READWRITE : VIRTUALMEM_READONLY,
        nullptr, nullptr);
    if( pVMem == nullptr )
    {
        if( EQUAL(pszImpl, "NO") || EQUAL(pszImpl, "OFF") ||
            EQUAL(pszImpl, "0") || EQUAL(pszImpl, "FALSE") )
        {
            return nullptr;
        }
        return GDALRasterBand::GetVirtualMemAuto(eRWFlag, pnPixelSpace,
                                                 pnLineSpace, papszOptions);
    }

    *pnPixelSpace = nPixelOffset;
    *pnLineSpace = nLineOffset;
    return pVMem;
}

/************************************************************************/
/* ==================================================================== */
/*      RawDataset                                                      */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            RawDataset()                              */
/************************************************************************/

RawDataset::RawDataset() {}

/************************************************************************/
/*                           ~RawDataset()                              */
/************************************************************************/

// It's pure virtual function but must be defined, even if empty.
RawDataset::~RawDataset() {}

/************************************************************************/
/*                             IRasterIO()                              */
/*                                                                      */
/*      Multi-band raster io handler.                                   */
/************************************************************************/

CPLErr RawDataset::IRasterIO( GDALRWFlag eRWFlag,
                              int nXOff, int nYOff, int nXSize, int nYSize,
                              void *pData, int nBufXSize, int nBufYSize,
                              GDALDataType eBufType,
                              int nBandCount, int *panBandMap,
                              GSpacing nPixelSpace, GSpacing nLineSpace,
                              GSpacing nBandSpace,
                              GDALRasterIOExtraArg* psExtraArg )

{
    const char* pszInterleave = nullptr;

    // The default GDALDataset::IRasterIO() implementation would go to
    // BlockBasedRasterIO if the dataset is interleaved. However if the
    // access pattern is compatible with DirectIO() we don't want to go
    // BlockBasedRasterIO, but rather used our optimized path in
    // RawRasterBand::IRasterIO().
    if (nXSize == nBufXSize && nYSize == nBufYSize && nBandCount > 1 &&
        (pszInterleave = GetMetadataItem("INTERLEAVE",
                                         "IMAGE_STRUCTURE")) != nullptr &&
        EQUAL(pszInterleave, "PIXEL"))
    {
        int iBandIndex = 0;
        for( ; iBandIndex < nBandCount; iBandIndex++ )
        {
            RawRasterBand *poBand = dynamic_cast<RawRasterBand *>(
                GetRasterBand(panBandMap[iBandIndex]));
            if( poBand == nullptr ||
                !poBand->CanUseDirectIO(nXOff, nYOff,
                                        nXSize, nYSize, eBufType, psExtraArg) )
            {
                break;
            }
        }
        if( iBandIndex == nBandCount )
        {
            GDALProgressFunc pfnProgressGlobal = psExtraArg->pfnProgress;
            void *pProgressDataGlobal = psExtraArg->pProgressData;

            CPLErr eErr = CE_None;
            for( iBandIndex = 0;
                 iBandIndex < nBandCount && eErr == CE_None;
                 iBandIndex++ )
            {
                GDALRasterBand *poBand = GetRasterBand(panBandMap[iBandIndex]);

                if (poBand == nullptr)
                {
                    eErr = CE_Failure;
                    break;
                }

                GByte *pabyBandData =
                    static_cast<GByte *>(pData) + iBandIndex * nBandSpace;

                psExtraArg->pfnProgress = GDALScaledProgress;
                psExtraArg->pProgressData = GDALCreateScaledProgress(
                    1.0 * iBandIndex / nBandCount,
                    1.0 * (iBandIndex + 1) / nBandCount, pfnProgressGlobal,
                    pProgressDataGlobal);

                eErr = poBand->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                        static_cast<void *>(pabyBandData),
                                        nBufXSize, nBufYSize, eBufType,
                                        nPixelSpace, nLineSpace, psExtraArg);

                GDALDestroyScaledProgress(psExtraArg->pProgressData);
            }

            psExtraArg->pfnProgress = pfnProgressGlobal;
            psExtraArg->pProgressData = pProgressDataGlobal;

            return eErr;
        }
    }

    return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                  nBufXSize, nBufYSize, eBufType, nBandCount,
                                  panBandMap, nPixelSpace, nLineSpace,
                                  nBandSpace, psExtraArg);
}


/************************************************************************/
/*                  RAWDatasetCheckMemoryUsage()                        */
/************************************************************************/

bool RAWDatasetCheckMemoryUsage(int nXSize, int nYSize, int nBands,
                                int nDTSize,
                                int nPixelOffset,
                                int nLineOffset,
                                vsi_l_offset nHeaderSize,
                                vsi_l_offset nBandOffset,
                                VSILFILE* fp)
{

    // Currently each RawRasterBand allocates nPixelOffset * nRasterXSize bytes
    // so for a pixel interleaved scheme, this will allocate lots of memory!
    // Actually this is quadratic in the number of bands!
    // Do a few sanity checks to avoid excessive memory allocation on
    // small files.
    // But ultimately we should fix RawRasterBand to have a shared buffer
    // among bands.
    const char* pszCheck = CPLGetConfigOption("RAW_CHECK_FILE_SIZE", nullptr);
    if( (nBands > 10 ||
         static_cast<vsi_l_offset>(nPixelOffset) * nXSize > 20000 ||
         (pszCheck && CPLTestBool(pszCheck))) &&
        !(pszCheck && !CPLTestBool(pszCheck)) )
    {
        vsi_l_offset nExpectedFileSize;
        try
        {
            nExpectedFileSize =
                (CPLSM(static_cast<GUInt64>(nHeaderSize)) +
                CPLSM(static_cast<GUInt64>(nBandOffset)) * CPLSM(static_cast<GUInt64>(nBands - 1)) +
                (nLineOffset >= 0 ? CPLSM(static_cast<GUInt64>(nYSize-1)) * CPLSM(static_cast<GUInt64>(nLineOffset)) : CPLSM(static_cast<GUInt64>(0))) +
                (nPixelOffset >= 0 ? CPLSM(static_cast<GUInt64>(nXSize-1)) * CPLSM(static_cast<GUInt64>(nPixelOffset)) : CPLSM(static_cast<GUInt64>(0)))).v();
        }
        catch( ... )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Image file is too small");
            return false;
        }
        CPL_IGNORE_RET_VAL( VSIFSeekL(fp, 0, SEEK_END) );
        vsi_l_offset nFileSize = VSIFTellL(fp);
        // Do not strictly compare against nExpectedFileSize, but use an arbitrary
        // 50% margin, since some raw formats such as ENVI
        // allow for sparse files (see https://github.com/OSGeo/gdal/issues/915)
        if( nFileSize < nExpectedFileSize / 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Image file is too small");
            return false;
        }
    }

    // Currently each RawRasterBand need to allocate nLineSize
    GIntBig nLineSize =
        static_cast<GIntBig>(std::abs(nPixelOffset)) * (nXSize - 1) + nDTSize;
    constexpr int knMAX_BUFFER_MEM = INT_MAX / 4;
    if( nBands > 0 && nLineSize > knMAX_BUFFER_MEM / nBands )
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "Too much memory needed");
        return false;
    }

    return true;
}

/************************************************************************/
/*                        GetRawBinaryLayout()                          */
/************************************************************************/

bool RawDataset::GetRawBinaryLayout(GDALDataset::RawBinaryLayout& sLayout)
{
    vsi_l_offset nImgOffset = 0;
    GIntBig nBandOffset = 0;
    int nPixelOffset = 0;
    int nLineOffset = 0;
    int bNativeOrder = 0;
    GDALDataType eDT = GDT_Unknown;
    for( int i = 1; i <= nBands; i++ )
    {
        auto poBand = dynamic_cast<RawRasterBand*>(GetRasterBand(i));
        if( poBand == nullptr )
            return false;
        if( i == 1 )
        {
            nImgOffset = poBand->nImgOffset;
            nPixelOffset = poBand->nPixelOffset;
            nLineOffset = poBand->nLineOffset;
            bNativeOrder = poBand->bNativeOrder;
            eDT = poBand->GetRasterDataType();
        }
        else if( nPixelOffset != poBand->nPixelOffset ||
                 nLineOffset != poBand->nLineOffset ||
                 bNativeOrder != poBand->bNativeOrder ||
                 eDT != poBand->GetRasterDataType() )
        {
            return false;
        }
        else if( i == 2 )
        {
            nBandOffset = static_cast<GIntBig>(poBand->nImgOffset) -
                                static_cast<GIntBig>(nImgOffset);
        }
        else if( nBandOffset * (i - 1) !=
                    static_cast<GIntBig>(poBand->nImgOffset) -
                        static_cast<GIntBig>(nImgOffset) )
        {
            return false;
        }
    }

    sLayout.eInterleaving = RawBinaryLayout::Interleaving::UNKNOWN;
    const int nDTSize = GDALGetDataTypeSizeBytes(eDT);
    if( nBands > 1 )
    {
        if( nPixelOffset == nBands * nDTSize &&
            nLineOffset == nPixelOffset * nRasterXSize &&
            nBandOffset == nDTSize )
        {
            sLayout.eInterleaving = RawBinaryLayout::Interleaving::BIP;
        }
        else if( nPixelOffset == nDTSize &&
                 nLineOffset == nDTSize * nBands * nRasterXSize &&
                 nBandOffset == static_cast<GIntBig>(nDTSize) * nRasterXSize )
        {
            sLayout.eInterleaving = RawBinaryLayout::Interleaving::BIL;
        }
        else if( nPixelOffset == nDTSize &&
                 nLineOffset == nDTSize * nRasterXSize &&
                 nBandOffset == static_cast<GIntBig>(nLineOffset) * nRasterYSize )
        {
            sLayout.eInterleaving = RawBinaryLayout::Interleaving::BSQ;
        }
    }

    sLayout.eDataType = eDT;
#ifdef CPL_LSB
    sLayout.bLittleEndianOrder = CPL_TO_BOOL(bNativeOrder);
#else
    sLayout.bLittleEndianOrder = CPL_TO_BOOL(!bNativeOrder);
#endif
    sLayout.nImageOffset = nImgOffset;
    sLayout.nPixelOffset = nPixelOffset;
    sLayout.nLineOffset = nLineOffset;
    sLayout.nBandOffset = nBandOffset;

    return true;
}
