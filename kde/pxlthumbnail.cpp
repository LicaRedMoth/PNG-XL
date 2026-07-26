#include "pxlthumbnail.h"
#include "pxl_handler.h"

#include <KPluginFactory>

#include <QFile>
#include <QImage>

PxlThumbnailCreator::PxlThumbnailCreator(QObject* parent, const QVariantList& args)
    : KIO::ThumbnailCreator(parent, args)
{
}

KIO::ThumbnailResult PxlThumbnailCreator::create(const KIO::ThumbnailRequest& request)
{
    QFile file(request.url().toLocalFile());
    if (!file.open(QIODevice::ReadOnly))
        return KIO::ThumbnailResult::fail();

    /* Reuse the Qt image plugin's own handler rather than re-implementing
       pxl_image/apxl_anim -> QImage conversion here. For .apxl this reads
       just the first frame, which is what a static thumbnail needs. */
    PxlHandler handler;
    handler.setDevice(&file);

    QImage image;
    if (!handler.read(&image) || image.isNull())
        return KIO::ThumbnailResult::fail();

    return KIO::ThumbnailResult::pass(image);
}

K_PLUGIN_CLASS_WITH_JSON(PxlThumbnailCreator, "pxlthumbnail.json")

#include "pxlthumbnail.moc"
