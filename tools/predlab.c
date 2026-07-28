/** \file predlab.c
    \brief Испытательный стенд для кандидатов предсказателей (вне формата).

    Грузит PNG штатным загрузчиком, применяет варианты преобразований к
    пиксельному буферу и сжимает результат zstd. Печатает только размеры,
    чтобы сравнивать кандидатов между собой и с текущей базовой линией.
    Формат .pxl этот инструмент не читает и не пишет.

    Использование: predlab [-l LEVEL] FILE.png [FILE.png ...]
*/
#include "pxl.h"
#include "pxl_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

/*----------------------------------------------------------------------------
  Предсказатели: значение предсказывается по уже восстановленным соседям
  a = слева, b = сверху, c = сверху-слева.
----------------------------------------------------------------------------*/

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p  = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* MED / LOCO-I: предсказатель из JPEG-LS. */
static uint8_t med(uint8_t a, uint8_t b, uint8_t c)
{
    int mx, mn;
    if (a > b) { mx = a; mn = b; } else { mx = b; mn = a; }
    if (c >= mx) return (uint8_t)mn;
    if (c <= mn) return (uint8_t)mx;
    return (uint8_t)((int)a + (int)b - (int)c);
}

/* Градиент с половинным весом диагонали: (a + b) / 2 ходит хуже на резких
   краях, эта форма ближе к GAP из CALIC. */
static uint8_t grad(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (((int)b - (int)c) >> 1);
    if (p < 0) p = 0;
    if (p > 255) p = 255;
    return (uint8_t)p;
}

typedef enum {
    PRED_LEFT = 0,
    PRED_UP,
    PRED_AVG,
    PRED_PAETH,
    PRED_MED,
    PRED_GRAD,
    PRED_COUNT
} pred_kind;

static const char* pred_name(pred_kind k)
{
    switch (k) {
        case PRED_LEFT:  return "left";
        case PRED_UP:    return "up";
        case PRED_AVG:   return "avg";
        case PRED_PAETH: return "paeth";
        case PRED_MED:   return "med";
        case PRED_GRAD:  return "grad";
        default:         return "?";
    }
}

static uint8_t predict(pred_kind k, uint8_t a, uint8_t b, uint8_t c)
{
    switch (k) {
        case PRED_LEFT:  return a;
        case PRED_UP:    return b;
        case PRED_AVG:   return (uint8_t)(((int)a + (int)b) >> 1);
        case PRED_PAETH: return paeth(a, b, c);
        case PRED_MED:   return med(a, b, c);
        case PRED_GRAD:  return grad(a, b, c);
        default:         return 0;
    }
}

/*----------------------------------------------------------------------------
  Преобразования буфера. Все работают по байтам с шагом pixel_bytes, так что
  подходят и для 8-битных, и (побайтово) для 16-битных данных.
----------------------------------------------------------------------------*/

/* Чередующийся вывод: остаток на месте пикселя, предсказание по каналам. */
static void filter_interleaved(const uint8_t* in, uint8_t* out,
                               uint32_t w, uint32_t h, unsigned pb,
                               pred_kind k)
{
    size_t stride = (size_t)w * pb;
    uint32_t y, x;
    unsigned i;
    for (y = 0; y < h; ++y) {
        const uint8_t* row  = in + (size_t)y * stride;
        const uint8_t* prow = y ? row - stride : NULL;
        uint8_t*       orow = out + (size_t)y * stride;
        for (x = 0; x < w; ++x) {
            for (i = 0; i < pb; ++i) {
                size_t o = (size_t)x * pb + i;
                uint8_t a = x ? row[o - pb] : 0;
                uint8_t b = prow ? prow[o] : 0;
                uint8_t c = (x && prow) ? prow[o - pb] : 0;
                orow[o] = (uint8_t)(row[o] - predict(k, a, b, c));
            }
        }
    }
}

/* Планарный вывод: сначала все остатки канала 0, потом канала 1 и т.д.
   Предсказание идёт по исходным значениям того же канала. */
static void filter_planar(const uint8_t* in, uint8_t* out,
                          uint32_t w, uint32_t h, unsigned pb,
                          pred_kind k)
{
    size_t stride = (size_t)w * pb;
    size_t plane  = (size_t)w * h;
    unsigned i;
    uint32_t y, x;
    for (i = 0; i < pb; ++i) {
        uint8_t* op = out + plane * i;
        for (y = 0; y < h; ++y) {
            const uint8_t* row  = in + (size_t)y * stride;
            const uint8_t* prow = y ? row - stride : NULL;
            for (x = 0; x < w; ++x) {
                size_t o = (size_t)x * pb + i;
                uint8_t a = x ? row[o - pb] : 0;
                uint8_t b = prow ? prow[o] : 0;
                uint8_t c = (x && prow) ? prow[o - pb] : 0;
                op[(size_t)y * w + x] = (uint8_t)(row[o] - predict(k, a, b, c));
            }
        }
    }
}

/* Обратимое цветовое преобразование, как в текущем BCIF: Y=b, U=g-b, V=g-r.
   Применяется к исходным пикселям, затем предсказание идёт уже по
   декоррелированным каналам, планарно. Это и есть главный кандидат:
   текущий BCIF делает наоборот (сначала left-delta, потом цвет). */
static void filter_ycocg_planar(const uint8_t* in, uint8_t* out,
                                uint32_t w, uint32_t h, unsigned pb,
                                pred_kind k, int full_ycocg)
{
    size_t stride = (size_t)w * pb;
    size_t plane  = (size_t)w * h;
    uint8_t* tmp = (uint8_t*)malloc(plane * pb);
    unsigned i;
    uint32_t y, x;
    if (!tmp) return;

    /* Шаг 1: цветовое преобразование в плоскости. */
    for (y = 0; y < h; ++y) {
        const uint8_t* row = in + (size_t)y * stride;
        for (x = 0; x < w; ++x) {
            const uint8_t* p = row + (size_t)x * pb;
            uint8_t c0, c1, c2;
            if (full_ycocg) {
                /* YCoCg-R, полностью обратимое, Y в 8 битах. */
                uint8_t co = (uint8_t)(p[0] - p[2]);
                uint8_t t  = (uint8_t)(p[2] + (co >> 1));
                uint8_t cg = (uint8_t)(p[1] - t);
                c0 = (uint8_t)(t + (cg >> 1));
                c1 = co;
                c2 = cg;
            } else {
                c0 = p[2];
                c1 = (uint8_t)(p[1] - p[2]);
                c2 = (uint8_t)(p[1] - p[0]);
            }
            tmp[plane * 0 + (size_t)y * w + x] = c0;
            tmp[plane * 1 + (size_t)y * w + x] = c1;
            tmp[plane * 2 + (size_t)y * w + x] = c2;
            for (i = 3; i < pb; ++i) {
                tmp[plane * i + (size_t)y * w + x] = p[i];
            }
        }
    }

    /* Шаг 2: предсказание внутри каждой плоскости. */
    for (i = 0; i < pb; ++i) {
        const uint8_t* ip = tmp + plane * i;
        uint8_t*       op = out + plane * i;
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                uint8_t a = x ? ip[(size_t)y * w + x - 1] : 0;
                uint8_t b = y ? ip[(size_t)(y - 1) * w + x] : 0;
                uint8_t c = (x && y) ? ip[(size_t)(y - 1) * w + x - 1] : 0;
                op[(size_t)y * w + x] =
                    (uint8_t)(ip[(size_t)y * w + x] - predict(k, a, b, c));
            }
        }
    }
    free(tmp);
}

/* Текущий BCIF: left-delta по чередующимся каналам, затем цвет, затем планы.
   Воспроизведён здесь, чтобы стенд мерил его в тех же условиях. */
static void filter_bcif_current(const uint8_t* in, uint8_t* out,
                                uint32_t w, uint32_t h, unsigned pb)
{
    size_t plane = (size_t)w * h;
    uint32_t y, x;
    unsigned i;
    for (y = 0; y < h; ++y) {
        uint8_t prev[8] = { 0 };
        for (x = 0; x < w; ++x) {
            const uint8_t* p = in + ((size_t)y * w + x) * pb;
            uint8_t d[8];
            for (i = 0; i < pb; ++i) {
                d[i] = (uint8_t)(p[i] - prev[i]);
                prev[i] = p[i];
            }
            out[plane * 0 + (size_t)y * w + x] = d[2];
            out[plane * 1 + (size_t)y * w + x] = (uint8_t)(d[1] - d[2]);
            out[plane * 2 + (size_t)y * w + x] = (uint8_t)(d[1] - d[0]);
            for (i = 3; i < pb; ++i) {
                out[plane * i + (size_t)y * w + x] = d[i];
            }
        }
    }
}

/*----------------------------------------------------------------------------
  Замер
----------------------------------------------------------------------------*/

static size_t squeeze(const uint8_t* data, size_t size, int level)
{
    size_t bound = ZSTD_compressBound(size);
    void* work = malloc(bound);
    size_t got;
    if (!work) return 0;
    got = ZSTD_compress(work, bound, data, size, level);
    free(work);
    return ZSTD_isError(got) ? 0 : got;
}

typedef struct {
    char   name[32];
    size_t total;
} slot;

#define MAX_SLOTS 64

static slot slots[MAX_SLOTS];
static int  n_slots;

static void record(const char* name, size_t bytes)
{
    int i;
    for (i = 0; i < n_slots; ++i) {
        if (strcmp(slots[i].name, name) == 0) {
            slots[i].total += bytes;
            return;
        }
    }
    if (n_slots < MAX_SLOTS) {
        snprintf(slots[n_slots].name, sizeof(slots[n_slots].name), "%s", name);
        slots[n_slots].total = bytes;
        ++n_slots;
    }
}

/* Размер файла на диске, чтобы сравнивать с исходным PNG. */
static size_t file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    n = ftell(f);
    fclose(f);
    return n < 0 ? 0 : (size_t)n;
}

int main(int argc, char** argv)
{
    int level = 12;
    int argi = 1;
    size_t png_total = 0;
    int files = 0;

    if (argi + 1 < argc && strcmp(argv[argi], "-l") == 0) {
        level = atoi(argv[argi + 1]);
        argi += 2;
    }
    if (argi >= argc) {
        fprintf(stderr, "использование: predlab [-l LEVEL] FILE.png ...\n");
        return 2;
    }

    printf("file\tw\th\tch\tvariant\tbytes\n");

    for (; argi < argc; ++argi) {
        const char* path = argv[argi];
        pxl_image img = pxl_load_png(path);
        size_t raw, fsize, png_size;
        unsigned pb;
        uint8_t* buf;
        pred_kind k;

        if (!img.buffer.data) {
            fprintf(stderr, "пропуск (не загрузился): %s\n", path);
            continue;
        }
        /* Стенд работает по байтам; сюда пускаем только полнобайтовые данные. */
        if (img.bit_depth && img.bit_depth < 8) {
            fprintf(stderr, "пропуск (глубина %u): %s\n", img.bit_depth, path);
            pxl_image_free(&img);
            continue;
        }

        pb  = (unsigned)img.channels * img.bytes_per_channel;
        raw = (size_t)img.width * img.height * pb;
        if (!raw || pb > 8 || raw != img.buffer.size) {
            fprintf(stderr, "пропуск (геометрия): %s\n", path);
            pxl_image_free(&img);
            continue;
        }

        png_size = file_size(path);

        buf = (uint8_t*)malloc(raw);
        if (!buf) { pxl_image_free(&img); continue; }

        ++files;
        png_total += png_size;

        /* Опорная точка: без фильтра. */
        fsize = squeeze(img.buffer.data, raw, level);
        printf("%s\t%u\t%u\t%u\traw\t%zu\n", path, img.width, img.height,
               img.channels, fsize);
        record("raw", fsize);

        /* Текущий BCIF (только для 3/4 каналов, 8 бит). */
        if (img.bytes_per_channel == 1 && img.channels >= 3) {
            filter_bcif_current(img.buffer.data, buf, img.width, img.height, pb);
            fsize = squeeze(buf, raw, level);
            printf("%s\t%u\t%u\t%u\tbcif-current\t%zu\n", path, img.width,
                   img.height, img.channels, fsize);
            record("bcif-current", fsize);
        }

        for (k = 0; k < PRED_COUNT; ++k) {
            char name[64];

            filter_interleaved(img.buffer.data, buf, img.width, img.height, pb, k);
            fsize = squeeze(buf, raw, level);
            snprintf(name, sizeof(name), "il-%s", pred_name(k));
            printf("%s\t%u\t%u\t%u\t%s\t%zu\n", path, img.width, img.height,
                   img.channels, name, fsize);
            record(name, fsize);

            filter_planar(img.buffer.data, buf, img.width, img.height, pb, k);
            fsize = squeeze(buf, raw, level);
            snprintf(name, sizeof(name), "pl-%s", pred_name(k));
            printf("%s\t%u\t%u\t%u\t%s\t%zu\n", path, img.width, img.height,
                   img.channels, name, fsize);
            record(name, fsize);

            if (img.bytes_per_channel == 1 && img.channels >= 3) {
                filter_ycocg_planar(img.buffer.data, buf, img.width, img.height,
                                    pb, k, 0);
                fsize = squeeze(buf, raw, level);
                snprintf(name, sizeof(name), "sub-%s", pred_name(k));
                printf("%s\t%u\t%u\t%u\t%s\t%zu\n", path, img.width, img.height,
                       img.channels, name, fsize);
                record(name, fsize);

                filter_ycocg_planar(img.buffer.data, buf, img.width, img.height,
                                    pb, k, 1);
                fsize = squeeze(buf, raw, level);
                snprintf(name, sizeof(name), "ycocg-%s", pred_name(k));
                printf("%s\t%u\t%u\t%u\t%s\t%zu\n", path, img.width, img.height,
                       img.channels, name, fsize);
                record(name, fsize);
            }
        }

        free(buf);
        pxl_image_free(&img);
    }

    fprintf(stderr, "\n=== итог по %d файлам (уровень %d) ===\n", files, level);
    fprintf(stderr, "%-16s %14s %8s\n", "variant", "bytes", "vs png");
    fprintf(stderr, "%-16s %14zu %7.2f%%\n", "png(исходный)", png_total, 100.0);
    {
        int i;
        for (i = 0; i < n_slots; ++i) {
            fprintf(stderr, "%-16s %14zu %7.2f%%\n", slots[i].name,
                    slots[i].total,
                    png_total ? 100.0 * (double)slots[i].total / (double)png_total
                              : 0.0);
        }
    }
    return 0;
}
