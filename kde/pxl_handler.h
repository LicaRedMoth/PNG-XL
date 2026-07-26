#ifndef PXL_HANDLER_H
#define PXL_HANDLER_H

#include <QImageIOHandler>

extern "C" {
#include <apxl.h>
#include <pxl.h>
}

/* QImageIOHandler for .pxl (still) and .apxl (animation). Decodes via
   libpxlcore only -- no libpng dependency, since pxl_decode/apxl_decode
   already hand back raw pixels. Read-only: encoding stays pxltool's job. */
class PxlHandler : public QImageIOHandler
{
public:
    PxlHandler();
    ~PxlHandler() override;

    bool canRead() const override;
    bool read(QImage* image) override;

    int imageCount() const override;
    int currentImageNumber() const override;
    int nextImageDelay() const override;
    int loopCount() const override;
    bool jumpToImage(int imageNumber) override;
    bool jumpToNextImage() override;

    bool supportsOption(ImageOption option) const override;
    QVariant option(ImageOption option) const override;

    /* Peek-only magic sniffing, shared with the plugin's capabilities(). */
    static bool looksLikePxl(const QByteArray& head);
    static bool looksLikeApxl(const QByteArray& head);

private:
    enum class Kind { Unknown, Still, Animated };

    /* const so imageCount()/supportsOption() can trigger the decode too --
       QImageReader/QMovie query those before the first read() to learn
       whether the file is animated at all. */
    bool ensureDecoded() const;

    mutable Kind m_kind = Kind::Unknown;
    mutable bool m_decodeAttempted = false;
    mutable bool m_decodeOk = false;
    int m_currentImage = 0;

    mutable pxl_image m_still{};
    mutable apxl_anim m_anim{};
};

#endif /* PXL_HANDLER_H */
