#include "pxl_handler.h"

#include <QIODevice>
#include <QSize>
#include <QtEndian>

extern "C" {
#include <apxl_format.h>
#include <pxl_format.h>
}

#include <cstring>

bool PxlHandler::looksLikePxl(const QByteArray& head)
{
    pxl_header h;
    return pxl_header_read(reinterpret_cast<const unsigned char*>(head.constData()),
                            static_cast<size_t>(head.size()), &h) != 0;
}

bool PxlHandler::looksLikeApxl(const QByteArray& head)
{
    apxl_file_header h;
    return apxl_header_read(reinterpret_cast<const unsigned char*>(head.constData()),
                             static_cast<size_t>(head.size()), &h) != 0;
}

PxlHandler::PxlHandler() = default;

PxlHandler::~PxlHandler()
{
    if (m_kind == Kind::Still)
        pxl_image_free(&m_still);
    else if (m_kind == Kind::Animated)
        apxl_free(&m_anim);
}

bool PxlHandler::canRead() const
{
    /* Once decoded, ensureDecoded() has already consumed the device via
       readAll() -- peeking it again would see EOF. Report readiness from the
       decoded state instead, which is also what lets read() be called
       repeatedly across animation frames. */
    if (m_decodeAttempted)
        return m_decodeOk && m_currentImage >= 0
            && (m_kind == Kind::Still
                || static_cast<uint32_t>(m_currentImage) < m_anim.frame_count);

    QIODevice* dev = device();
    if (!dev)
        return false;
    const QByteArray head = dev->peek(APXL_HEADER_BYTES);
    return looksLikePxl(head) || looksLikeApxl(head);
}

/* Converts one full-canvas pxl_image into an owned QImage. 16-bit samples are
   stored PNG-native big-endian in pxl_image (see pxl.h); Qt's 16-bit formats
   are host-endian, so those paths byte-swap on the way out. */
static QImage imageFromPxl(const pxl_image& img)
{
    const uint32_t w = img.width, h = img.height;
    const unsigned char* src = img.buffer.data;

    if (img.bytes_per_channel == 1) {
        switch (img.channels) {
        case 1:
            return QImage(src, w, h, w, QImage::Format_Grayscale8).copy();
        case 3:
            return QImage(src, w, h, w * 3, QImage::Format_RGB888).copy();
        case 4:
            return QImage(src, w, h, w * 4, QImage::Format_RGBA8888).copy();
        case 2: {
            /* Grey+alpha has no direct QImage format; expand to RGBA8888. */
            QImage out(w, h, QImage::Format_RGBA8888);
            for (uint32_t y = 0; y < h; ++y) {
                const unsigned char* row = src + static_cast<size_t>(y) * w * 2;
                auto* dst = reinterpret_cast<uchar*>(out.scanLine(y));
                for (uint32_t x = 0; x < w; ++x) {
                    const unsigned char g = row[x * 2 + 0];
                    const unsigned char a = row[x * 2 + 1];
                    dst[x * 4 + 0] = g;
                    dst[x * 4 + 1] = g;
                    dst[x * 4 + 2] = g;
                    dst[x * 4 + 3] = a;
                }
            }
            return out;
        }
        default:
            return QImage();
        }
    }

    /* bytes_per_channel == 2 */
    switch (img.channels) {
    case 1: {
        QImage out(w, h, QImage::Format_Grayscale16);
        for (uint32_t y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const quint16*>(src + static_cast<size_t>(y) * w * 2);
            auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
            for (uint32_t x = 0; x < w; ++x)
                dst[x] = qFromBigEndian(row[x]);
        }
        return out;
    }
    case 2: {
        QImage out(w, h, QImage::Format_RGBA64);
        for (uint32_t y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const quint16*>(src + static_cast<size_t>(y) * w * 4);
            auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
            for (uint32_t x = 0; x < w; ++x) {
                const quint16 g = qFromBigEndian(row[x * 2 + 0]);
                const quint16 a = qFromBigEndian(row[x * 2 + 1]);
                dst[x * 4 + 0] = g;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = g;
                dst[x * 4 + 3] = a;
            }
        }
        return out;
    }
    case 3: {
        QImage out(w, h, QImage::Format_RGBA64);
        for (uint32_t y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const quint16*>(src + static_cast<size_t>(y) * w * 3 * 2);
            auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
            for (uint32_t x = 0; x < w; ++x) {
                dst[x * 4 + 0] = qFromBigEndian(row[x * 3 + 0]);
                dst[x * 4 + 1] = qFromBigEndian(row[x * 3 + 1]);
                dst[x * 4 + 2] = qFromBigEndian(row[x * 3 + 2]);
                dst[x * 4 + 3] = 0xFFFF;
            }
        }
        return out;
    }
    case 4: {
        QImage out(w, h, QImage::Format_RGBA64);
        for (uint32_t y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const quint16*>(src + static_cast<size_t>(y) * w * 4 * 2);
            auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
            for (uint32_t x = 0; x < w * 4; ++x)
                dst[x] = qFromBigEndian(row[x]);
        }
        return out;
    }
    default:
        return QImage();
    }
}

bool PxlHandler::ensureDecoded() const
{
    if (m_decodeAttempted)
        return m_decodeOk;
    m_decodeAttempted = true;

    QIODevice* dev = device();
    if (!dev)
        return false;

    const QByteArray bytes = dev->readAll();
    pxl_buffer file{reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.constData())),
                     static_cast<size_t>(bytes.size())};

    const QByteArray head = bytes.left(APXL_HEADER_BYTES);
    if (looksLikeApxl(head)) {
        m_anim = apxl_decode(file);
        if (!m_anim.frames || m_anim.frame_count == 0)
            return false;
        m_kind = Kind::Animated;
        m_decodeOk = true;
        return true;
    }
    if (looksLikePxl(head)) {
        m_still = pxl_decode(file);
        if (!m_still.buffer.data)
            return false;
        m_kind = Kind::Still;
        m_decodeOk = true;
        return true;
    }
    return false;
}

bool PxlHandler::read(QImage* image)
{
    if (!ensureDecoded())
        return false;

    if (m_kind == Kind::Still) {
        *image = imageFromPxl(m_still);
        return !image->isNull();
    }

    if (m_currentImage < 0 || static_cast<uint32_t>(m_currentImage) >= m_anim.frame_count)
        return false;
    *image = imageFromPxl(m_anim.frames[m_currentImage].image);
    return !image->isNull();
}

int PxlHandler::imageCount() const
{
    /* QImageReader/QMovie call this before the first read() to decide
       whether the file is animated at all -- decode eagerly rather than
       reporting 1 just because nothing has read a frame yet. */
    ensureDecoded();
    if (m_kind != Kind::Animated)
        return 1;
    return static_cast<int>(m_anim.frame_count);
}

int PxlHandler::currentImageNumber() const
{
    return m_currentImage;
}

int PxlHandler::nextImageDelay() const
{
    if (m_kind != Kind::Animated || m_currentImage < 0
        || static_cast<uint32_t>(m_currentImage) >= m_anim.frame_count)
        return 0;
    const apxl_frame& f = m_anim.frames[m_currentImage];
    const uint16_t den = f.delay_den == 0 ? 100 : f.delay_den;
    return static_cast<int>((1000u * f.delay_num) / den);
}

int PxlHandler::loopCount() const
{
    if (m_kind != Kind::Animated)
        return 0;
    return m_anim.loop_count == 0 ? -1 : static_cast<int>(m_anim.loop_count) - 1;
}

bool PxlHandler::jumpToImage(int imageNumber)
{
    if (!ensureDecoded())
        return false;
    if (m_kind != Kind::Animated) {
        m_currentImage = 0;
        return imageNumber == 0;
    }
    if (imageNumber < 0 || static_cast<uint32_t>(imageNumber) >= m_anim.frame_count)
        return false;
    m_currentImage = imageNumber;
    return true;
}

bool PxlHandler::jumpToNextImage()
{
    if (!ensureDecoded())
        return false;
    if (m_kind != Kind::Animated)
        return false;
    if (static_cast<uint32_t>(m_currentImage + 1) >= m_anim.frame_count)
        return false;
    ++m_currentImage;
    return true;
}

bool PxlHandler::supportsOption(ImageOption option) const
{
    return option == QImageIOHandler::Animation || option == QImageIOHandler::Size;
}

QVariant PxlHandler::option(ImageOption option) const
{
    if (option == QImageIOHandler::Animation) {
        ensureDecoded();
        return m_kind == Kind::Animated;
    }
    if (option == QImageIOHandler::Size) {
        /* QMovie sizes its internal frame buffer off this option before the
           first read() -- without it, it composites into a 0x0 image even
           though read() itself decodes fine. */
        if (!ensureDecoded())
            return QVariant();
        if (m_kind == Kind::Still)
            return QSize(static_cast<int>(m_still.width), static_cast<int>(m_still.height));
        if (m_kind == Kind::Animated && m_anim.frame_count > 0)
            return QSize(static_cast<int>(m_anim.frames[0].image.width),
                         static_cast<int>(m_anim.frames[0].image.height));
        return QVariant();
    }
    return QVariant();
}
