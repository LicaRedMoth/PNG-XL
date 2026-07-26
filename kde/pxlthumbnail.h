#ifndef PXLTHUMBNAIL_H
#define PXLTHUMBNAIL_H

#include <kio/thumbnailcreator.h>

/* Dolphin's own thumbnailer (kio-extras' imagethumbnail.so) has a MimeTypes
   list baked into its metadata at kio-extras' build time, so it never routes
   .pxl/.apxl to QImageReader no matter what image plugins are installed.
   This is a standalone KIO::ThumbnailCreator that declares image/x-pxl and
   image/x-apxl itself and decodes via the same PxlHandler used by the Qt
   image plugin. */
class PxlThumbnailCreator : public KIO::ThumbnailCreator
{
public:
    PxlThumbnailCreator(QObject* parent, const QVariantList& args);

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest& request) override;
};

#endif /* PXLTHUMBNAIL_H */
