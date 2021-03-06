/*------------------------------------
  ///\  Plywood C++ Framework
  \\\/  https://plywood.arc80.com/
------------------------------------*/
#include <ply-runtime/Precomp.h>
#include <ply-runtime/io/text/TextConverter.h>
#include <ply-runtime/io/OutStream.h>

namespace ply {

//-----------------------------------------------------------------------
// TextConverter
//-----------------------------------------------------------------------
PLY_NO_INLINE TextConverter::TextConverter(const TextEncoding* dstEncoding,
                                           const TextEncoding* srcEncoding)
    : dstEncoding{dstEncoding}, srcEncoding{srcEncoding} {
}

PLY_NO_INLINE bool TextConverter::convert(BufferView* dstBuf, ConstBufferView* srcBuf, bool flush) {
    bool didWork = false;

    auto flushDstSmallBuf = [&] {
        u32 numBytesToCopy = min((u32) this->dstSmallBuf.numBytes, dstBuf->numBytes);
        if (numBytesToCopy > 0) {
            memcpy(dstBuf->bytes, this->dstSmallBuf.bytes, numBytesToCopy);
            this->dstSmallBuf.popFront(numBytesToCopy);
            dstBuf->offsetHead(numBytesToCopy);
            didWork = true;
        }
        return numBytesToCopy;
    };

    while (this->dstSmallBuf.numBytes > 0 || this->srcSmallBuf.numBytes > 0) {
        // First, try to copy any bytes that have been buffered into dstSmallBuf.
        flushDstSmallBuf();
        if (dstBuf->numBytes == 0)
            return didWork; // dstBuf has been filled.

        // If we get here, dstSmallBuf has been emptied.
        PLY_ASSERT(this->dstSmallBuf.numBytes == 0);

        // If nothing in srcSmallBuf, break here and enter main loop below.
        if (this->srcSmallBuf.numBytes == 0)
            break; // Nothing in srcSmallBuf

        // If we get here, some truncated data was left in srcSmallBuf.
        // Append more input to srcSmallBuf.
        u32 smallBufInitialNumBytes = this->srcSmallBuf.numBytes;
        u32 numBytesToAppend =
            min((u32) PLY_STATIC_ARRAY_SIZE(this->srcSmallBuf.bytes) - this->srcSmallBuf.numBytes,
                srcBuf->numBytes);
        memcpy(this->srcSmallBuf.bytes + this->srcSmallBuf.numBytes, srcBuf->bytes,
               numBytesToAppend);
        this->srcSmallBuf.numBytes += numBytesToAppend;

        // Try to decode from srcSmallBuf.
        DecodeResult decoded = this->srcEncoding->decodePoint(this->srcSmallBuf.view());
        PLY_ASSERT(decoded.point >= 0);
        if (decoded.status == DecodeResult::Status::Truncated && !flush) {
            // Not enough input units for a complete code point
            PLY_ASSERT(this->srcSmallBuf.numBytes < 4);       // Sanity check
            return didWork;
        }

        // We've got enough input bytes for a complete code point. Consume them.
        srcBuf->offsetHead(decoded.numBytes - smallBufInitialNumBytes);
        this->srcSmallBuf.popFront(decoded.numBytes);
        didWork = true;

        // Encode this code point to dstSmallBuf
        this->dstSmallBuf.numBytes = this->dstEncoding->encodePoint(
            {this->dstSmallBuf.bytes, PLY_STATIC_ARRAY_SIZE(this->dstSmallBuf.bytes)},
            decoded.point);
        PLY_ASSERT(this->dstSmallBuf.numBytes > 0);

        // Iterate again through this loop so that dstSmallBuf gets flushed.
    }

    // At this point, both dstSmallBuf and srcSmallBuf should be empty, which means that we can now
    // operate directly on srcBuf and dstBuf.
    PLY_ASSERT(dstBuf->numBytes > 0);
    PLY_ASSERT(this->dstSmallBuf.numBytes == 0);
    PLY_ASSERT(this->srcSmallBuf.numBytes == 0);

    while (srcBuf->numBytes > 0) {
        // Decode one point from the input.
        DecodeResult decoded = this->srcEncoding->decodePoint(*srcBuf);
        PLY_ASSERT(decoded.point >= 0);
        didWork = true;

        if (decoded.status == DecodeResult::Status::Truncated && !flush) {
            // Not enough input units for a complete code point. Copy input to srcSmallBuf.
            PLY_ASSERT(srcBuf->numBytes < 4);                 // Sanity check
            memcpy(this->srcSmallBuf.bytes, srcBuf->bytes, srcBuf->numBytes);
            this->srcSmallBuf.numBytes = srcBuf->numBytes;
            srcBuf->offsetHead(srcBuf->numBytes);
            return didWork;
        }

        // Consume input bytes.
        srcBuf->offsetHead(decoded.numBytes);

        if (dstBuf->numBytes >= 4) {
            // Encode directly to the output buffer.
            u32 numBytesEncoded = this->dstEncoding->encodePoint(*dstBuf, decoded.point);
            PLY_ASSERT(numBytesEncoded > 0);
            dstBuf->offsetHead(numBytesEncoded);
        } else {
            // Encode to dstSmallBuf.
            this->dstSmallBuf.numBytes = this->dstEncoding->encodePoint(
                {this->dstSmallBuf.bytes, PLY_STATIC_ARRAY_SIZE(this->dstSmallBuf.bytes)},
                decoded.point);
            PLY_ASSERT(this->dstSmallBuf.numBytes > 0);

            // Flush dstSmallBuf.
            flushDstSmallBuf();
            if (dstBuf->numBytes == 0)
                return didWork; // dstBuf has been filled.
        }
    }

    // No more input.
    return didWork;
}

PLY_NO_INLINE bool TextConverter::writeTo(OutStream* outs, ConstBufferView* srcBuf, bool flush) {
    bool anyWorkDone = false;
    for (;;) {
        if (!outs->tryMakeBytesAvailable())
            break;

        // Write as much output as we can
        BufferView dstBuf = outs->viewAvailable();
        bool didWork = this->convert(&dstBuf, srcBuf, flush);
        outs->curByte = dstBuf.bytes;
        if (!didWork)
            break;
        anyWorkDone = true;
    }
    return anyWorkDone;
}

PLY_NO_INLINE u32 TextConverter::readFrom(InStream* ins, BufferView* dstBuf) {
    u32 totalBytesWritten = 0;
    for (;;) {
        // FIXME: Make this algorithm tighter!
        // Advancing the input is a potentially blocking operation, so only do it if we
        // absolutely have to:
        ins->tryMakeBytesAvailable(4); // will return less than 4 on EOF/error *ONLY*

        // Filter as much input as we can:
        u8* dstBefore = dstBuf->bytes;
        ConstBufferView srcBuf = ins->viewAvailable();
        bool flush = ins->atEOF();
        this->convert(dstBuf, &srcBuf, flush);
        ins->curByte = (u8*) srcBuf.bytes;
        s32 numBytesWritten = safeDemote<s32>(dstBuf->bytes - dstBefore);
        totalBytesWritten += numBytesWritten;

        // If anything was written, stop.
        if (numBytesWritten > 0)
            break;
        // If input was exhausted, stop.
        if (flush) {
            PLY_ASSERT(ins->numBytesAvailable() == 0);
            break;
        }
    }
    return totalBytesWritten;
}

PLY_NO_INLINE Buffer TextConverter::convertInternal(const TextEncoding* dstEncoding,
                                                    const TextEncoding* srcEncoding,
                                                    ConstBufferView srcText) {
    MemOutStream outs;
    TextConverter converter{dstEncoding, srcEncoding};
    converter.writeTo(&outs, &srcText, true);
    PLY_ASSERT(dstEncoding->unitSize > 0);
    return outs.moveToBuffer();
}

//-----------------------------------------------------------------------
// InPipe_TextConverter
//-----------------------------------------------------------------------
PLY_NO_INLINE void InPipe_TextConverter_destroy(InPipe* inPipe_) {
    InPipe_TextConverter* inPipe = static_cast<InPipe_TextConverter*>(inPipe_);
    destruct(inPipe->ins);
}

PLY_NO_INLINE u32 InPipe_TextConverter_readSome(InPipe* inPipe_, BufferView dstBuf) {
    InPipe_TextConverter* inPipe = static_cast<InPipe_TextConverter*>(inPipe_);
    return inPipe->converter.readFrom(inPipe->ins, &dstBuf);
}

InPipe::Funcs InPipe_TextConverter::Funcs_ = {
    InPipe_TextConverter_destroy,
    InPipe_TextConverter_readSome,
    InPipe::getFileSize_Unsupported,
};

PLY_NO_INLINE InPipe_TextConverter::InPipe_TextConverter(OptionallyOwned<InStream>&& ins,
                                                         const TextEncoding* dstEncoding,
                                                         const TextEncoding* srcEncoding)
    : InPipe{&Funcs_}, ins{std::move(ins)}, converter{dstEncoding, srcEncoding} {
}

//-----------------------------------------------------------------------
// OutPipe_TextConverter
//-----------------------------------------------------------------------
PLY_NO_INLINE void OutPipe_TextConverter_destroy(OutPipe* outPipe_) {
    OutPipe_TextConverter* outPipe = static_cast<OutPipe_TextConverter*>(outPipe_);
    destruct(outPipe->outs);
}

PLY_NO_INLINE bool OutPipe_TextConverter_write(OutPipe* outPipe_, ConstBufferView srcBuf) {
    OutPipe_TextConverter* outPipe = static_cast<OutPipe_TextConverter*>(outPipe_);
    outPipe->converter.writeTo(outPipe->outs, &srcBuf, false);
    return !outPipe->outs->atEOF();
}

PLY_NO_INLINE bool OutPipe_TextConverter_flush(OutPipe* outPipe_, bool toDevice) {
    OutPipe_TextConverter* outPipe = static_cast<OutPipe_TextConverter*>(outPipe_);
    ConstBufferView emptySrcBuf;
    outPipe->converter.writeTo(outPipe->outs, &emptySrcBuf, true);
    return outPipe->outs->flush(toDevice);
}

OutPipe::Funcs OutPipe_TextConverter::Funcs_ = {
    OutPipe_TextConverter_destroy,
    OutPipe_TextConverter_write,
    OutPipe_TextConverter_flush,
    OutPipe::seek_Empty,
};

PLY_NO_INLINE OutPipe_TextConverter::OutPipe_TextConverter(OptionallyOwned<OutStream>&& outs,
                                                           const TextEncoding* dstEncoding,
                                                           const TextEncoding* srcEncoding)
    : OutPipe{&Funcs_}, outs{std::move(outs)}, converter{dstEncoding, srcEncoding} {
}

} // namespace ply
