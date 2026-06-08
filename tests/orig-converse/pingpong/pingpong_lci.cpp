/***************************************************************
  LCI-only Ping-pong to test the message latency and bandwidth
  Matches the converse ping-pong size sweep and warm-up behavior.
 ****************************************************************/

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "lci.hpp"

struct Config {
  int n_cycles = 1000;
  size_t min_msg_size = 1 << 9;
  size_t max_msg_size = 1 << 14;
  size_t factor = 2;
} g_config;

static void die(const std::string& message)
{
  if (lci::get_rank_me() == 0) {
    std::cerr << message << std::endl;
  }
  std::abort();
}

static void parse_args(int argc, char** argv)
{
  if (argc >= 5) {
    g_config.n_cycles = std::stoi(argv[1]);
    g_config.min_msg_size = static_cast<size_t>(std::stoul(argv[2]));
    g_config.max_msg_size = static_cast<size_t>(std::stoul(argv[3]));
    g_config.factor = static_cast<size_t>(std::stoul(argv[4]));
  } else if (argc != 1) {
    die("Usage: ./pingpong_lci <ncycles> <minsize> <maxsize> <increase factor>\nExample: ./pingpong_lci 100 2 128 2");
  }

  if (g_config.n_cycles <= 0) {
    die("ncycles must be positive");
  }
  if (g_config.min_msg_size == 0 || g_config.max_msg_size == 0) {
    die("message sizes must be positive");
  }
  if (g_config.min_msg_size > g_config.max_msg_size) {
    die("minsize must not exceed maxsize");
  }
  if (g_config.factor < 2) {
    die("increase factor must be >= 2");
  }
}

static lci::status_t recv_message(lci::device_t device, lci::comp_t cq)
{
  lci::status_t status;
  do {
    lci::progress_x().device(device)();
    status = lci::cq_pop(cq);
  } while (status.is_retry());
  return status;
}

static void send_message(int peer_rank,
                         void* buffer,
                         size_t size,
                         lci::device_t device,
                         lci::rcomp_t rcomp)
{
  lci::status_t status;
  do {
    status = lci::post_am_x(peer_rank, buffer, size, lci::COMP_NULL_RETRY, rcomp)
                 .device(device)();
    lci::progress_x().device(device)();
  } while (status.is_retry());
}

static void verify_message(const lci::status_t& status,
                          size_t expected_size,
                          int expected_rank)
{
  assert(status.size == expected_size);
  assert(status.rank == expected_rank);
  for (size_t i = 0; i < expected_size; ++i) {
    assert(static_cast<unsigned char>(static_cast<char*>(status.buffer)[i]) ==
           static_cast<unsigned char>(expected_rank));
  }
}

static void pingpong_once(int rank,
                          int peer_rank,
                          const void* send_buf,
                          size_t msg_size,
                          lci::device_t device,
                          lci::comp_t cq,
                          lci::rcomp_t rcomp)
{
  if (rank == 0) {
    send_message(peer_rank, const_cast<void*>(send_buf), msg_size, device, rcomp);
    lci::status_t status = recv_message(device, cq);
    verify_message(status, msg_size, peer_rank);
    free(status.buffer);
  } else {
    lci::status_t status = recv_message(device, cq);
    verify_message(status, msg_size, peer_rank);
    free(status.buffer);
    send_message(peer_rank, const_cast<void*>(send_buf), msg_size, device, rcomp);
  }
}

int main(int argc, char** argv)
{
  lci::g_runtime_init_x().alloc_default_device(false)();

  int rank = lci::get_rank_me();
  int nranks = lci::get_rank_n();
  if (nranks != 2) {
    die("This test is designed for exactly 2 ranks");
  }

  parse_args(argc, argv);

  if (rank == 0) {
    std::cout << "Pingpong with iterations = " << g_config.n_cycles
              << ", minMsgSize = " << g_config.min_msg_size
              << ", maxMsgSize = " << g_config.max_msg_size
              << ", increase factor = " << g_config.factor << std::endl;
  }

  lci::device_t device = lci::alloc_device();
  lci::comp_t cq = lci::alloc_cq();
  lci::rcomp_t rcomp = lci::register_rcomp(cq);
  lci::barrier_x().device(device)();

  int peer_rank = rank ^ 1;

  for (size_t msg_size = g_config.min_msg_size;;) {
    std::vector<char> send_buf(msg_size, static_cast<char>(rank));

    pingpong_once(rank, peer_rank, send_buf.data(), msg_size, device, cq, rcomp);

    std::chrono::high_resolution_clock::time_point start_time;
    if (rank == 0) {
      start_time = std::chrono::high_resolution_clock::now();
    }

    for (int cycle = 0; cycle < g_config.n_cycles; ++cycle) {
      pingpong_once(rank, peer_rank, send_buf.data(), msg_size, device, cq, rcomp);
    }

    if (rank == 0) {
      auto end_time = std::chrono::high_resolution_clock::now();
      double elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time)
              .count();
      std::cout << "Size=" << msg_size
                << " bytes, time="
                << (elapsed_us / (2.0 * g_config.n_cycles))
                << " microseconds one-way" << std::endl;
    }

    if (msg_size == g_config.max_msg_size) {
      break;
    }

    size_t next_msg_size = msg_size * g_config.factor;
    if (next_msg_size < msg_size || next_msg_size > g_config.max_msg_size) {
      next_msg_size = g_config.max_msg_size;
    }
    msg_size = next_msg_size;
  }

  lci::free_comp(&cq);
  lci::free_device(&device);
  lci::g_runtime_fina();
  return 0;
}