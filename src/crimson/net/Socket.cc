// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Socket.h"

#include "crimson/common/log.h"
#include "Errors.h"

namespace ceph::net {

namespace {

seastar::logger& logger() {
  return ceph::get_logger(ceph_subsys_ms);
}

// an input_stream consumer that reads buffer segments into a bufferlist up to
// the given number of remaining bytes
struct bufferlist_consumer {
  bufferlist& bl;
  size_t& remaining;

  bufferlist_consumer(bufferlist& bl, size_t& remaining)
    : bl(bl), remaining(remaining) {}

  using tmp_buf = seastar::temporary_buffer<char>;
  using consumption_result_type = typename seastar::input_stream<char>::consumption_result_type;

  // consume some or all of a buffer segment
  seastar::future<consumption_result_type> operator()(tmp_buf&& data) {
    if (remaining >= data.size()) {
      // consume the whole buffer
      remaining -= data.size();
      bl.append(buffer::create_foreign(std::move(data)));
      if (remaining > 0) {
        // return none to request more segments
        return seastar::make_ready_future<consumption_result_type>(
            seastar::continue_consuming{});
      } else {
        // return an empty buffer to singal that we're done
        return seastar::make_ready_future<consumption_result_type>(
            consumption_result_type::stop_consuming_type({}));
      }
    }
    if (remaining > 0) {
      // consume the front
      bl.append(buffer::create_foreign(data.share(0, remaining)));
      data.trim_front(remaining);
      remaining = 0;
    }
    // give the rest back to signal that we're done
    return seastar::make_ready_future<consumption_result_type>(
        consumption_result_type::stop_consuming_type{std::move(data)});
  };
};

} // anonymous namespace

seastar::future<bufferlist> Socket::read(size_t bytes)
{
  if (bytes == 0) {
    return seastar::make_ready_future<bufferlist>();
  }
  r.buffer.clear();
  r.remaining = bytes;
  return in.consume(bufferlist_consumer{r.buffer, r.remaining})
    .then([this] {
      if (r.remaining) { // throw on short reads
        throw std::system_error(make_error_code(error::read_eof));
      }
      return seastar::make_ready_future<bufferlist>(std::move(r.buffer));
    });
}

seastar::future<seastar::temporary_buffer<char>>
Socket::read_exactly(size_t bytes) {
  if (bytes == 0) {
    return seastar::make_ready_future<seastar::temporary_buffer<char>>();
  }
  return in.read_exactly(bytes)
    .then([this](auto buf) {
      if (buf.empty()) {
        throw std::system_error(make_error_code(error::read_eof));
      }
      return seastar::make_ready_future<tmp_buf>(std::move(buf));
    });
}

void Socket::shutdown() {
#ifndef NDEBUG
  ceph_assert(!down);
  down = true;
#endif
  socket.shutdown_input();
  socket.shutdown_output();
}

static inline seastar::future<> close_and_handle_errors(auto& out) {
  return out.close().handle_exception_type([] (const std::system_error& e) {
    if (e.code() != error::broken_pipe &&
        e.code() != error::connection_reset) {
      logger().error("Socket::close(): unexpected error {}", e);
      ceph_abort();
    }
    // can happen when out is already shutdown, ignore
  });
}

seastar::future<> Socket::close() {
#ifndef NDEBUG
  ceph_assert(!closed);
  closed = true;
#endif
  return seastar::when_all_succeed(
    in.close(),
    close_and_handle_errors(out)
  ).handle_exception([] (auto eptr) {
    logger().error("Socket::close(): unexpected exception {}", eptr);
    ceph_abort();
  });
}

} // namespace ceph::net
