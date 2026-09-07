find_path(FFmpeg_INCLUDE_DIR libavcodec/avcodec.h)
find_library(FFmpeg_AVCODEC_LIBRARY NAMES avcodec)
find_library(FFmpeg_AVUTIL_LIBRARY NAMES avutil)
find_library(FFmpeg_SWSCALE_LIBRARY NAMES swscale)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_INCLUDE_DIR FFmpeg_AVCODEC_LIBRARY
                  FFmpeg_AVUTIL_LIBRARY FFmpeg_SWSCALE_LIBRARY)

if(FFmpeg_FOUND AND NOT TARGET FFmpeg::avcodec)
    add_library(FFmpeg::avutil UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::avutil PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_AVUTIL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}")

    add_library(FFmpeg::avcodec UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::avcodec PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_AVCODEC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES FFmpeg::avutil)

    add_library(FFmpeg::swscale UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::swscale PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_SWSCALE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES FFmpeg::avutil)
endif()

mark_as_advanced(FFmpeg_INCLUDE_DIR FFmpeg_AVCODEC_LIBRARY
                 FFmpeg_AVUTIL_LIBRARY FFmpeg_SWSCALE_LIBRARY)
