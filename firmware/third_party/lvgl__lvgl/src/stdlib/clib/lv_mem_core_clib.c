/**
 * @file lv_mem_core_clib.c
 */

/*********************
 *      INCLUDES
 *********************/

#include "../../lvgl_public.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CLIB

#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/
/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_mem_init(void)
{
    return; /*Nothing to init*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit*/

}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    /*Not supported*/
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    /*Not supported*/
    LV_UNUSED(pool);
    return;
}

void * lv_malloc_core(size_t size)
{
#ifdef ESP_PLATFORM
    /* ORB PATCH: widgets/styles and temporary draw buffers are CPU-only. The
     * generic <=1 KiB policy and its exhaustion fallback can consume DMA-capable
     * RAM during the first online redraw. Keep that heap for radio/display;
     * a failed UI allocation must never silently borrow its reserve. */
    return orb_memory_alloc(ORB_MEMORY_LVGL, size);
#else
    return malloc(size);
#endif
}

void * lv_realloc_core(void * p, size_t new_size)
{
#ifdef ESP_PLATFORM
    return orb_memory_realloc(ORB_MEMORY_LVGL, p, new_size);
#else
    return realloc(p, new_size);
#endif
}

void lv_free_core(void * p)
{
#ifdef ESP_PLATFORM
    orb_memory_free(p);
#else
    free(p);
#endif
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    /*Not supported*/
    LV_UNUSED(mon_p);
    return;
}

lv_result_t lv_mem_test_core(void)
{
    /*Not supported*/
    return LV_RESULT_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_STDLIB_CLIB*/
