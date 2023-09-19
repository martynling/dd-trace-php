use crate::bindings as zend;
use libc::{c_char, c_int};
use log::debug;
use std::ffi::CStr;
use std::time::Instant;

pub type VmIoCustomReadFn = unsafe extern "C" fn(*mut zend::_php_stream, *mut i8, usize) -> isize;

pub fn io_profiling_startup() {
    unsafe {
        let url_stream_wrappers_hash = zend::php_stream_get_url_stream_wrappers_hash_global();
        let wrapper = zend::zend_hash_str_find_ptr_lc(
            url_stream_wrappers_hash,
            b"file\0".as_ptr() as *const _,
            4,
        ) as *mut zend::php_stream_wrapper;

        if let Some(stream_opener) = (*(*wrapper).wops).stream_opener {
            // Create a copy of wops
            let new_wops: *mut zend::_php_stream_wrapper_ops =
                Box::into_raw(Box::new((*(*wrapper).wops).clone()));

            // Save the original opener in the global variable
            ORIG_STREAM_OPENER = Some(stream_opener);

            // Change the stream_opener to the profiled version
            (*new_wops).stream_opener = Some(profiled_stream_opener);

            // Assign the new wops back to the wrapper
            (*wrapper).wops = new_wops as *const zend::_php_stream_wrapper_ops;
        }
    }
}

pub type VmIoCustomStreamOpenerFn = unsafe extern "C" fn(
    wrapper: *mut zend::php_stream_wrapper,
    filename: *const c_char,
    mode: *const c_char,
    options: c_int,
    opened_path: *mut *mut zend::zend_string,
    context: *mut zend::php_stream_context,
) -> *mut zend::php_stream;

static mut ORIG_READ_HANDLER: Option<VmIoCustomReadFn> = None;

static mut ORIG_STREAM_OPENER: Option<VmIoCustomStreamOpenerFn> = None;

unsafe extern "C" fn profiled_stream_opener(
    wrapper: *mut zend::php_stream_wrapper,
    filename: *const c_char,
    mode: *const c_char,
    options: c_int,
    opened_path: *mut *mut zend::zend_string,
    context: *mut zend::php_stream_context,
) -> *mut zend::php_stream {
    let stream =
        ORIG_STREAM_OPENER.unwrap()(wrapper, filename, mode, options, opened_path, context);

    let new_stream_ops: *mut zend::_php_stream_ops =
        Box::into_raw(Box::new((*(*stream).ops).clone()));

    ORIG_READ_HANDLER = (*(*stream).ops).read;

    (*new_stream_ops).read = Some(io_profiling_plainfile_read);

    (*stream).ops = new_stream_ops;

    return stream;
}

unsafe extern "C" fn io_profiling_plainfile_read(
    stream: *mut zend::php_stream,
    buf: *mut c_char,
    count: usize,
) -> isize {
    let start = Instant::now();
    let result = (ORIG_READ_HANDLER.unwrap())(stream, buf, count);
    let duration = start.elapsed();
    debug!(
        "File IO took: {} nanoseconds ({} microseconds, {} milliseconds) and read {result} bytes (reqeusted {count} bytes). Path: {:?}",
        duration.as_nanos(),
        duration.as_micros(),
        duration.as_millis(),
        CStr::from_ptr((*stream).orig_path)
    );
    result
}
