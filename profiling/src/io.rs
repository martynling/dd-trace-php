use crate::zend::{self, zend_execute_data, zval, InternalFunctionHandler};
use crate::{PROFILER, REQUEST_LOCALS};
use log::warn;
use std::ffi::CStr;
use std::time::Instant;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

fn try_io_wait(
    func: unsafe extern "C" fn(execute_data: *mut zend_execute_data, return_value: *mut zval),
    reason: &'static str,
    execute_data: *mut zend_execute_data,
    return_value: *mut zval,
) -> anyhow::Result<()> {
    // let timeline_enabled = REQUEST_LOCALS.with(|cell| {
    //     cell.try_borrow()
    //         .map(|locals| locals.profiling_experimental_timeline_enabled)
    //         .unwrap_or(false)
    // });

    // if !timeline_enabled {
    //     unsafe { func(execute_data, return_value) };
    //     return Ok(());
    // }

    let start = Instant::now();

    // SAFETY: simple forwarding to original func with original args.
    unsafe { func(execute_data, return_value) };

    let duration = start.elapsed();

    // > Returns an Err if earlier is later than self, and the error contains
    // > how far from self the time is.
    // This shouldn't ever happen (now is always later than the epoch) but in
    // case it does, short-circuit the function.
    let now = SystemTime::now().duration_since(UNIX_EPOCH)?;

    // Consciously not holding request locals/profiler during the forwarded
    // call. If they are, then it's possible to get a deadlock/bad borrow
    // because the call triggers something to happen like a time/allocation
    // sample and the extension tries to re-acquire these.
    REQUEST_LOCALS.with(|cell| {
        // try to borrow and bail out if not successful
        let locals = cell.try_borrow()?;

        match PROFILER.lock() {
            Ok(guard) => match guard.as_ref() {
                Some(profiler) => profiler.collect_io_wait(
                    unsafe { zend::ddog_php_prof_get_current_execute_data() },
                    now.as_nanos() as i64,
                    duration.as_nanos() as i64,
                    reason,
                    &locals,
                ),
                None => { /* Profiling is probably disabled, no worries */ }
            },
            Err(err) => anyhow::bail!("profiler mutex: {err:#}"),
        }
        Ok(())
    })
}

macro_rules! create_ddog_php_prof_function_and_handler {
    ($base_name:ident, $reason:expr) => {
        // Constructing the handler and function names
        paste::item! {
            static mut [<$base_name:upper _ORIG_HANDLER>]: InternalFunctionHandler = None;

            #[no_mangle]
            unsafe extern "C" fn [<ddog_php_prof_ $base_name _handler>](
                execute_data: *mut zend_execute_data,
                return_value: *mut zval,
            ) {
                if let Some(func) = unsafe { [<$base_name:upper _ORIG_HANDLER>] } {
                    if let Err(err) = try_io_wait(func, $reason, execute_data, return_value) {
                        warn!("error creating profiling timeline sample for an internal function: {err:#}");
                    }
                }
            }
        }
    };
}

create_ddog_php_prof_function_and_handler!(flock, "select");
create_ddog_php_prof_function_and_handler!(file_get_contents, "select");
create_ddog_php_prof_function_and_handler!(file_put_contents, "select");
create_ddog_php_prof_function_and_handler!(fopen, "select");
create_ddog_php_prof_function_and_handler!(fread, "select");
create_ddog_php_prof_function_and_handler!(file, "select");

/// This function is run during the STARTUP phase and hooks into the execution of some functions
/// that we'd like to observe in regards of visualization on the timeline
pub unsafe fn io_startup() {
    let handlers = [
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"flock\0"),
            &mut FLOCK_ORIG_HANDLER,
            Some(ddog_php_prof_flock_handler),
        ),
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"file_get_contents\0"),
            &mut FILE_GET_CONTENTS_ORIG_HANDLER,
            Some(ddog_php_prof_file_get_contents_handler),
        ),
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"file_put_contents\0"),
            &mut FILE_PUT_CONTENTS_ORIG_HANDLER,
            Some(ddog_php_prof_file_put_contents_handler),
        ),
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"fopen\0"),
            &mut FOPEN_ORIG_HANDLER,
            Some(ddog_php_prof_fopen_handler),
        ),
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"fread\0"),
            &mut FREAD_ORIG_HANDLER,
            Some(ddog_php_prof_fread_handler),
        ),
        zend::datadog_php_zif_handler::new(
            CStr::from_bytes_with_nul_unchecked(b"file\0"),
            &mut FILE_ORIG_HANDLER,
            Some(ddog_php_prof_file_handler),
        ),
    ];

    for handler in handlers.into_iter() {
        // Safety: we've set all the parameters correctly for this C call.
        zend::datadog_php_install_handler(handler);
    }
}
