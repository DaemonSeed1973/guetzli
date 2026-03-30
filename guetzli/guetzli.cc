/*
 * Copyright 2016 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <sstream>
#include <string.h>
#include <thread>
#include <vector>
#include "png.h"
#include "guetzli/jpeg_data.h"
#include "guetzli/jpeg_data_reader.h"
#include "guetzli/processor.h"
#include "guetzli/quality.h"
#include "guetzli/stats.h"

namespace {

constexpr int kDefaultJPEGQuality = 95;

// An upper estimate of memory usage of Guetzli. The bound is
// max(kLowerMemusaeMB * 1<<20, pixel_count * kBytesPerPixel)
constexpr int kBytesPerPixel = 350;
constexpr int kLowestMemusageMB = 100; // in MB

constexpr int kDefaultMemlimitMB = 6000; // in MB

inline uint8_t BlendOnBlack(const uint8_t val, const uint8_t alpha) {
  return (static_cast<int>(val) * static_cast<int>(alpha) + 128) / 255;
}

bool ReadPNG(const std::string& data, int* xsize, int* ysize,
             std::vector<uint8_t>* rgb) {
  png_structp png_ptr =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr) {
    return false;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return false;
  }

  if (setjmp(png_jmpbuf(png_ptr)) != 0) {
    // Ok we are here because of the setjmp.
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return false;
  }

  std::istringstream memstream(data, std::ios::in | std::ios::binary);
  png_set_read_fn(png_ptr, static_cast<void*>(&memstream), [](png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
    std::istringstream& memstream = *static_cast<std::istringstream*>(png_get_io_ptr(png_ptr));
    
    memstream.read(reinterpret_cast<char*>(outBytes), byteCountToRead);

    if (memstream.eof()) png_error(png_ptr, "unexpected end of data");
    if (memstream.fail()) png_error(png_ptr, "read from memory error");
  });

  // The png_transforms flags are as follows:
  // packing == convert 1,2,4 bit images,
  // strip == 16 -> 8 bits / channel,
  // shift == use sBIT dynamics, and
  // expand == palettes -> rgb, grayscale -> 8 bit images, tRNS -> alpha.
  const unsigned int png_transforms =
      PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16;

  png_read_png(png_ptr, info_ptr, png_transforms, nullptr);

  png_bytep* row_pointers = png_get_rows(png_ptr, info_ptr);

  *xsize = png_get_image_width(png_ptr, info_ptr);
  *ysize = png_get_image_height(png_ptr, info_ptr);
  rgb->resize(3 * (*xsize) * (*ysize));

  const int components = png_get_channels(png_ptr, info_ptr);
  switch (components) {
    case 1: {
      // GRAYSCALE
      for (int y = 0; y < *ysize; ++y) {
        const uint8_t* row_in = row_pointers[y];
        uint8_t* row_out = &(*rgb)[3 * y * (*xsize)];
        for (int x = 0; x < *xsize; ++x) {
          const uint8_t gray = row_in[x];
          row_out[3 * x + 0] = gray;
          row_out[3 * x + 1] = gray;
          row_out[3 * x + 2] = gray;
        }
      }
      break;
    }
    case 2: {
      // GRAYSCALE + ALPHA
      for (int y = 0; y < *ysize; ++y) {
        const uint8_t* row_in = row_pointers[y];
        uint8_t* row_out = &(*rgb)[3 * y * (*xsize)];
        for (int x = 0; x < *xsize; ++x) {
          const uint8_t gray = BlendOnBlack(row_in[2 * x], row_in[2 * x + 1]);
          row_out[3 * x + 0] = gray;
          row_out[3 * x + 1] = gray;
          row_out[3 * x + 2] = gray;
        }
      }
      break;
    }
    case 3: {
      // RGB
      for (int y = 0; y < *ysize; ++y) {
        const uint8_t* row_in = row_pointers[y];
        uint8_t* row_out = &(*rgb)[3 * y * (*xsize)];
        memcpy(row_out, row_in, 3 * (*xsize));
      }
      break;
    }
    case 4: {
      // RGBA
      for (int y = 0; y < *ysize; ++y) {
        const uint8_t* row_in = row_pointers[y];
        uint8_t* row_out = &(*rgb)[3 * y * (*xsize)];
        for (int x = 0; x < *xsize; ++x) {
          const uint8_t alpha = row_in[4 * x + 3];
          row_out[3 * x + 0] = BlendOnBlack(row_in[4 * x + 0], alpha);
          row_out[3 * x + 1] = BlendOnBlack(row_in[4 * x + 1], alpha);
          row_out[3 * x + 2] = BlendOnBlack(row_in[4 * x + 2], alpha);
        }
      }
      break;
    }
    default:
      png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
      return false;
  }
  png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
  return true;
}

std::string ReadFileOrDie(const char* filename) {
  bool read_from_stdin = strncmp(filename, "-", 2) == 0;

  FILE* f = read_from_stdin ? stdin : fopen(filename, "rb");
  if (!f) {
    perror("Can't open input file");
    exit(1);
  }

  std::string result;
  off_t buffer_size = 8192;

  if (fseek(f, 0, SEEK_END) == 0) {
    buffer_size = std::max<off_t>(ftell(f), 1);
    if (fseek(f, 0, SEEK_SET) != 0) {
      perror("fseek");
      exit(1);
    }
  } else if (ferror(f)) {
    perror("fseek");
    exit(1);
  }

  std::unique_ptr<char[]> buf(new char[buffer_size]);
  while (!feof(f)) {
    size_t read_bytes = fread(buf.get(), sizeof(char), buffer_size, f);
    if (ferror(f)) {
      perror("fread");
      exit(1);
    }
    result.append(buf.get(), read_bytes);
  }

  fclose(f);
  return result;
}

void WriteFileOrDie(const char* filename, const std::string& contents) {
  bool write_to_stdout = strncmp(filename, "-", 2) == 0;

  FILE* f = write_to_stdout ? stdout : fopen(filename, "wb");
  if (!f) {
    perror("Can't open output file for writing");
    exit(1);
  }
  if (fwrite(contents.data(), 1, contents.size(), f) != contents.size()) {
    perror("fwrite");
    exit(1);
  }
  if (fclose(f) < 0) {
    perror("fclose");
    exit(1);
  }
}

void TerminateHandler() {
  fprintf(stderr, "Unhandled exception. Most likely insufficient memory available.\n"
          "Make sure that there is 300MB/MPix of memory available.\n");
  exit(1);
}

bool ReadFile(const std::string& filename, std::string* out) {
  FILE* f = fopen(filename.c_str(), "rb");
  if (!f) return false;
  std::string result;
  off_t buffer_size = 8192;
  if (fseek(f, 0, SEEK_END) == 0) {
    buffer_size = std::max<off_t>(ftell(f), 1);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
  } else if (ferror(f)) { fclose(f); return false; }
  std::unique_ptr<char[]> buf(new char[buffer_size]);
  while (!feof(f)) {
    size_t read_bytes = fread(buf.get(), sizeof(char), buffer_size, f);
    if (ferror(f)) { fclose(f); return false; }
    result.append(buf.get(), read_bytes);
  }
  fclose(f);
  *out = std::move(result);
  return true;
}

bool WriteFile(const std::string& filename, const std::string& contents) {
  FILE* f = fopen(filename.c_str(), "wb");
  if (!f) return false;
  if (fwrite(contents.data(), 1, contents.size(), f) != contents.size()) {
    fclose(f); return false;
  }
  if (fclose(f) < 0) return false;
  return true;
}

static const unsigned char kPNGMagicBytes[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
};

bool ProcessFile(const std::string& input_path,
                 const std::string& output_path,
                 const guetzli::Params& params,
                 int memlimit_mb, int verbose) {
  std::string in_data;
  if (!ReadFile(input_path, &in_data)) {
    fprintf(stderr, "Error: can't open input file: %s\n", input_path.c_str());
    return false;
  }
  std::string out_data;
  guetzli::ProcessStats stats;
  if (verbose) {
    stats.debug_output_file = stderr;
  }
  if (in_data.size() >= 8 &&
      memcmp(in_data.data(), kPNGMagicBytes, sizeof(kPNGMagicBytes)) == 0) {
    int xsize, ysize;
    std::vector<uint8_t> rgb;
    if (!ReadPNG(in_data, &xsize, &ysize, &rgb)) {
      fprintf(stderr, "Error reading PNG data from: %s\n", input_path.c_str());
      return false;
    }
    double pixels = static_cast<double>(xsize) * ysize;
    if (memlimit_mb != -1
        && (pixels * kBytesPerPixel / (1 << 20) > memlimit_mb
            || memlimit_mb < kLowestMemusageMB)) {
      fprintf(stderr, "Memory limit would be exceeded for: %s\n", input_path.c_str());
      return false;
    }
    if (!guetzli::Process(params, &stats, rgb, xsize, ysize, &out_data)) {
      fprintf(stderr, "Guetzli processing failed for: %s\n", input_path.c_str());
      return false;
    }
  } else {
    guetzli::JPEGData jpg_header;
    if (!guetzli::ReadJpeg(in_data, guetzli::JPEG_READ_HEADER, &jpg_header)) {
      fprintf(stderr, "Error reading JPG data from: %s\n", input_path.c_str());
      return false;
    }
    double pixels = static_cast<double>(jpg_header.width) * jpg_header.height;
    if (memlimit_mb != -1
        && (pixels * kBytesPerPixel / (1 << 20) > memlimit_mb
            || memlimit_mb < kLowestMemusageMB)) {
      fprintf(stderr, "Memory limit would be exceeded for: %s\n", input_path.c_str());
      return false;
    }
    if (!guetzli::Process(params, &stats, in_data, &out_data)) {
      fprintf(stderr, "Guetzli processing failed for: %s\n", input_path.c_str());
      return false;
    }
  }
  if (!WriteFile(output_path, out_data)) {
    fprintf(stderr, "Error: can't write output file: %s\n", output_path.c_str());
    return false;
  }
  return true;
}

void Usage() {
  fprintf(stderr,
      "Guetzli JPEG compressor. Usage: \n"
      "guetzli [flags] input_filename output_filename\n"
      "guetzli --batch [flags] --outdir <dir> <input1> [<input2>...]\n"
      "guetzli --batch [flags] --outdir <dir> <input_directory>\n"
      "\n"
      "Flags:\n"
      "  --verbose    - Print a verbose trace of all attempts to standard output.\n"
      "  --quality Q  - Visual quality to aim for, expressed as a JPEG quality value.\n"
      "                 Default value is %d.\n"
      "  --memlimit M - Memory limit in MB. Guetzli will fail if unable to stay under\n"
      "                 the limit. Default limit is %d MB.\n"
      "  --nomemlimit - Do not limit memory usage.\n"
      "\n"
      "Batch mode flags:\n"
      "  --batch      - Enable batch mode.\n"
      "  --outdir D   - Output directory (required in batch mode).\n"
      "  --jobs N     - Max parallel jobs (default: hardware_concurrency, min 1).\n"
      "  --ext E      - When input is a directory, only process files with this\n"
      "                 extension (e.g. jpg, png). Can be repeated.\n",
      kDefaultJPEGQuality, kDefaultMemlimitMB);
  exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  std::set_terminate(TerminateHandler);

  int verbose = 0;
  int quality = kDefaultJPEGQuality;
  int memlimit_mb = kDefaultMemlimitMB;
  bool batch_mode = false;
  std::string outdir;
  int jobs = 0; // 0 means use hardware_concurrency
  std::vector<std::string> ext_filters;

  int opt_idx = 1;
  for(;opt_idx < argc;opt_idx++) {
    if (strnlen(argv[opt_idx], 2) < 2 || argv[opt_idx][0] != '-' || argv[opt_idx][1] != '-')
      break;
    if (!strcmp(argv[opt_idx], "--verbose")) {
      verbose = 1;
    } else if (!strcmp(argv[opt_idx], "--quality")) {
      opt_idx++;
      if (opt_idx >= argc)
        Usage();
      quality = atoi(argv[opt_idx]);
    } else if (!strcmp(argv[opt_idx], "--memlimit")) {
      opt_idx++;
      if (opt_idx >= argc)
        Usage();
      memlimit_mb = atoi(argv[opt_idx]);
    } else if (!strcmp(argv[opt_idx], "--nomemlimit")) {
      memlimit_mb = -1;
    } else if (!strcmp(argv[opt_idx], "--batch")) {
      batch_mode = true;
    } else if (!strcmp(argv[opt_idx], "--outdir")) {
      opt_idx++;
      if (opt_idx >= argc)
        Usage();
      outdir = argv[opt_idx];
    } else if (!strcmp(argv[opt_idx], "--jobs")) {
      opt_idx++;
      if (opt_idx >= argc)
        Usage();
      jobs = atoi(argv[opt_idx]);
      if (jobs < 1) jobs = 1;
    } else if (!strcmp(argv[opt_idx], "--ext")) {
      opt_idx++;
      if (opt_idx >= argc)
        Usage();
      std::string e = argv[opt_idx];
      // Normalize: strip leading dot if present
      if (!e.empty() && e[0] == '.') e = e.substr(1);
      // Lowercase
      std::transform(e.begin(), e.end(), e.begin(),
                     [](unsigned char c){ return std::tolower(c); });
      ext_filters.push_back(e);
    } else if (!strcmp(argv[opt_idx], "--")) {
      opt_idx++;
      break;
    } else {
      fprintf(stderr, "Unknown commandline flag: %s\n", argv[opt_idx]);
      Usage();
    }
  }

  guetzli::Params params;
  params.butteraugli_target = static_cast<float>(
      guetzli::ButteraugliScoreForQuality(quality));

  // --- Single-file mode (original behavior) ---
  if (!batch_mode) {
    if (argc - opt_idx != 2) {
      Usage();
    }
    bool ok = ProcessFile(argv[opt_idx], argv[opt_idx + 1],
                          params, memlimit_mb, verbose);
    return ok ? 0 : 1;
  }

  // --- Batch mode ---
  namespace fs = std::filesystem;

  if (outdir.empty()) {
    fprintf(stderr, "Error: --outdir is required in batch mode.\n");
    Usage();
  }
  if (argc - opt_idx < 1) {
    fprintf(stderr, "Error: no input files or directory specified.\n");
    Usage();
  }

  // Collect input files
  std::vector<std::string> input_files;

  // Check if single arg is a directory
  if (argc - opt_idx == 1 && fs::is_directory(argv[opt_idx])) {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(argv[opt_idx], ec)) {
      if (!entry.is_regular_file()) continue;
      std::string ext = entry.path().extension().string();
      if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c){ return std::tolower(c); });

      if (!ext_filters.empty()) {
        bool match = false;
        for (auto& f : ext_filters) {
          if (f == ext) { match = true; break; }
        }
        if (!match) continue;
      } else {
        // Default: only jpg/jpeg/png
        if (ext != "jpg" && ext != "jpeg" && ext != "png") continue;
      }
      input_files.push_back(entry.path().string());
    }
    std::sort(input_files.begin(), input_files.end());
  } else {
    for (int i = opt_idx; i < argc; ++i) {
      input_files.push_back(argv[i]);
    }
  }

  if (input_files.empty()) {
    fprintf(stderr, "Error: no input files found.\n");
    return 1;
  }

  // Create output directory
  {
    std::error_code ec;
    fs::create_directories(outdir, ec);
    if (ec) {
      fprintf(stderr, "Error: can't create output directory '%s': %s\n",
              outdir.c_str(), ec.message().c_str());
      return 1;
    }
  }

  // Generate output filenames with collision avoidance
  struct FileJob {
    std::string input;
    std::string output;
  };
  std::vector<FileJob> file_jobs;
  std::set<std::string> used_names;

  for (auto& inp : input_files) {
    fs::path p(inp);
    std::string ext = p.extension().string();
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::string stem;
    if (ext_lower == ".png") {
      stem = p.stem().string();
    } else {
      stem = p.stem().string() + ".guetzli";
    }
    std::string out_ext = ".jpg";

    std::string candidate = stem + out_ext;
    if (used_names.count(candidate)) {
      int suffix = 1;
      while (used_names.count(stem + "_" + std::to_string(suffix) + out_ext)) {
        suffix++;
      }
      candidate = stem + "_" + std::to_string(suffix) + out_ext;
    }
    used_names.insert(candidate);

    fs::path out_path = fs::path(outdir) / candidate;
    file_jobs.push_back({inp, out_path.string()});
  }

  // Thread pool
  int num_threads = jobs > 0 ? jobs
      : static_cast<int>(std::thread::hardware_concurrency());
  if (num_threads < 1) num_threads = 1;

  const size_t total = file_jobs.size();
  std::atomic<size_t> next_job{0};
  std::atomic<int> fail_count{0};
  std::mutex stderr_mutex;
  std::vector<std::string> failed_files;
  std::mutex failed_mutex;

  auto worker = [&]() {
    while (true) {
      size_t idx = next_job.fetch_add(1);
      if (idx >= total) break;
      auto& job = file_jobs[idx];

      {
        std::lock_guard<std::mutex> lock(stderr_mutex);
        fprintf(stderr, "[%zu/%zu] %s -> %s\n",
                idx + 1, total, job.input.c_str(), job.output.c_str());
      }

      bool ok = ProcessFile(job.input, job.output, params, memlimit_mb, verbose);
      if (!ok) {
        fail_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(failed_mutex);
        failed_files.push_back(job.input);
      }
    }
  };

  std::vector<std::thread> threads;
  int to_launch = std::min(num_threads, static_cast<int>(total));
  for (int i = 0; i < to_launch; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  // Summary
  int failures = fail_count.load();
  if (failures > 0) {
    fprintf(stderr, "\nBatch complete: %d/%zu failed:\n",
            failures, total);
    for (auto& f : failed_files) {
      fprintf(stderr, "  FAILED: %s\n", f.c_str());
    }
    return 1;
  }

  fprintf(stderr, "\nBatch complete: %zu/%zu succeeded.\n", total, total);
  return 0;
}
