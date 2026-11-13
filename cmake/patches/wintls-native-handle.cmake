# Adds stream::native_handle() to boost-wintls 1.0.0 so clients can pin the negotiated peer certificate.
# Runs as the FetchContent patch step in the extracted source directory.

function(beamit_patch file before after)
    file(READ "${file}" content)
    string(FIND "${content}" "${after}" applied)
    if(NOT applied EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "wintls patch context not found in ${file}")
    endif()
    string(REPLACE "${before}" "${after}" content "${content}")
    file(WRITE "${file}" "${content}")
endfunction()

set(handshake_before [[
  void set_certificate_revocation_check(bool check) {
    check_revocation_ = check;
  }
]])
set(handshake_after [[
  void set_certificate_revocation_check(bool check) {
    check_revocation_ = check;
  }

  CtxtHandle* native_handle() {
    return ctxt_handle_.get();
  }
]])
beamit_patch(include/wintls/detail/sspi_handshake.hpp "${handshake_before}" "${handshake_after}")

set(stream_before [[
  void set_certificate_revocation_check(bool check) {
    sspi_stream_->handshake.set_certificate_revocation_check(check);
  }
]])
set(stream_after [[
  void set_certificate_revocation_check(bool check) {
    sspi_stream_->handshake.set_certificate_revocation_check(check);
  }

  /** Get the SSPI security context, e.g. to query the peer certificate after the handshake. */
  CtxtHandle* native_handle() {
    return sspi_stream_->handshake.native_handle();
  }
]])
beamit_patch(include/wintls/stream.hpp "${stream_before}" "${stream_after}")
