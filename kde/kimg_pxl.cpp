#include "kimg_pxl.h"
#include "pxl_handler.h"

#include <QIODevice>

extern "C" {
#include <apxl_format.h>
}

QImageIOPlugin::Capabilities PxlPlugin::capabilities(QIODevice* device, const QByteArray& format) const
{
    if (format != "pxl" && format != "apxl" && !format.isEmpty())
        return {};

    if (!device)
        return format == "pxl" || format == "apxl" ? Capabilities(CanRead) : Capabilities();

    const QByteArray head = device->peek(APXL_HEADER_BYTES);
    if (PxlHandler::looksLikePxl(head) || PxlHandler::looksLikeApxl(head))
        return CanRead;
    return {};
}

QImageIOHandler* PxlPlugin::create(QIODevice* device, const QByteArray& format) const
{
    auto* handler = new PxlHandler();
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}
