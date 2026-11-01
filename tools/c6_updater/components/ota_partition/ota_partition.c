/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2026 Zapadenec1982
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP-Hosted Partition OTA Component
 * =================================
 *
 * Reads ESP32 slave firmware from dedicated flash partition and performs OTA update.
 *
 * FEATURES:
 * - Firmware validation (magic number, image header)
 * - Version checking against current slave firmware
 * - Direct partition reading with chunked transfer
 * - Robust error handling for partition access issues
 *
 * APIs USED:
 * - esp_hosted_get_coprocessor_fwversion() - Get slave firmware version
 * - esp_hosted_slave_ota_begin()  - Initialize OTA session
 * - esp_hosted_slave_ota_write()  - Transfer firmware chunks
 * - esp_hosted_slave_ota_end()    - Finalize OTA session
 * - esp_hosted_slave_ota_activate() - (if current slave FW > v2.5.X only) - ** Called from main.c **

 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_hosted_ota.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "mbedtls/sha256.h"
#include <stdbool.h>

static const char* TAG = "ota_partition";

#ifndef CHUNK_SIZE
#define CHUNK_SIZE 1500
#endif

/* Function to check if partition contains valid firmware data */
static esp_err_t check_partition_has_firmware(const esp_partition_t* partition)
{
	uint8_t buffer[256];
	esp_err_t ret;
	size_t total_checked = 0;
	size_t check_size = sizeof(buffer);

	ESP_LOGI(TAG, "Checking if partition '%s' contains firmware data...", partition->label);

	/* Check first 1KB of partition for any non-0xFF data */
	while (total_checked < 1024 && total_checked < partition->size) {
		check_size = (1024 - total_checked > sizeof(buffer)) ? sizeof(buffer) : (1024 - total_checked);

		ret = esp_partition_read(partition, total_checked, buffer, check_size);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to read partition data for validation: %s", esp_err_to_name(ret));
			return ret;
		}

		/* Check if all bytes are 0xFF (empty partition) */
		bool all_ff = true;
		for (size_t i = 0; i < check_size; i++) {
			if (buffer[i] != 0xFF) {
				all_ff = false;
				break;
			}
		}

		if (!all_ff) {
			ESP_LOGI(TAG, "Found non-empty data in partition at offset %u", (unsigned int)total_checked);
			return ESP_OK; /* Found some data */
		}

		total_checked += check_size;
	}

	if (total_checked >= 1024) {
		ESP_LOGW(TAG, "Partition appears to be empty or uninitialized (first 1KB is all 0xFF)!");
		ESP_LOGW(TAG, "");
		ESP_LOGW(TAG, "---- OPTION 1 ----");
		ESP_LOGW(TAG, "Keep Slave FW <here> and `idf.py fullclean` & `idf.py flash` again");
		ESP_LOGW(TAG, "  - host_performs_slave_ota/");
		ESP_LOGW(TAG, "     └── components/");
		ESP_LOGW(TAG, "          └── ota_partition/            # Slave OTA using Host Partition method");
		ESP_LOGW(TAG, "                └── slave_fw_bin/       # Put slave .bin files here");
		ESP_LOGW(TAG, "");
		ESP_LOGW(TAG, "       OR");
		ESP_LOGW(TAG, "");
		ESP_LOGW(TAG, "---- OPTION 2 ----");
		ESP_LOGW(TAG, "  1. Create a '%s' partition in your host partition table", partition->label);
		ESP_LOGW(TAG, "  2. Flashed desired slave firmware binary to this partition using 'idf.py partition-table-flash && idf.py app-flash' or similar");
		return ESP_ERR_NOT_FOUND;
	}

	return ESP_OK;
}

/* player-fork: verify the image in `slave_fw` before a single byte of it reaches the C6.
 *
 * WHY, in one sentence: the C6 is the panel's only network interface, the P4 is the only
 * thing that can reflash it, and an activated bad image leaves neither — recovery is a
 * USB-TTL adapter on JP1 with the P4 held in download mode (docs/C6_UPDATE.md, "Recovery:
 * flashing the C6 directly over UART (JP1)").
 *
 * What was here before parsed the image header for a SIZE and trusted every byte after it.
 * Nothing checked that the partition held the image it was supposed to hold. A download
 * that stopped, a copy into slave_fw_bin/ that was interrupted, a flash write that did not
 * take — all of them produce a partition whose first 24 bytes are a perfectly good header.
 *
 * An ESP application image carries two integrity fields of its own, and they are enough:
 *
 *   - a one-byte checksum, 0xEF xor every segment DATA byte, at the end of the last
 *     16-byte block of the image;
 *   - a SHA-256 over everything up to and including that byte, when hash_appended is set
 *     (it is, for anything esptool built this decade).
 *
 * The old size arithmetic could not have been used with either of them, because it was
 * wrong: it padded UP to a 16-byte boundary and then added the checksum byte, where esptool
 * pads so the checksum byte IS the last byte of the block, and it inserted another 16 bytes
 * of padding before the hash, where esptool inserts none. For any image whose segment data
 * does not end on a 16-byte boundary that is 16 bytes too many. It went unnoticed because
 * the slave's esp_ota_write() takes the extra bytes without complaint and esp_ota_end()
 * reads the real length out of the header. tools/c6_image.py has the layout written out,
 * and tools/tests/test_c6_image.py pins the arithmetic at eight different alignments.
 */

#define CP_SEED_CHECKSUM 0xEF
#define CP_SHA_LEN       32
#define CP_MAX_SEGMENTS  16
/* A segment longer than the C6's whole flash is not a segment, it is a misread field. */
#define CP_MAX_SEGMENT_LEN (4 * 1024 * 1024)

/* Read buffer for the two streaming passes. Static: the updater's main task has the default
 * stack and 1 KB of it is not free. */
static uint8_t s_cp_buf[1024];

typedef struct {
	size_t total_len;      /* bytes the image occupies, hash included */
	size_t hashed_len;     /* bytes the appended SHA-256 covers */
	int    segments;
	bool   hash_appended;
	bool   checksum_ok;
	char   version[32];
} cp_image_t;

/* Walk the segment table, XOR-ing the segment data as we go, and work out where the image
 * ends. Everything else is derived from this. */
static esp_err_t cp_image_scan(const esp_partition_t* part, cp_image_t* out)
{
	esp_image_header_t header;
	esp_err_t ret = esp_partition_read(part, 0, &header, sizeof(header));
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "cannot read the image header: %s", esp_err_to_name(ret));
		return ret;
	}
	if (header.magic != ESP_IMAGE_HEADER_MAGIC) {
		ESP_LOGE(TAG, "partition %s does not start with an ESP image (magic 0x%02X)",
				part->label, header.magic);
		return ESP_ERR_INVALID_ARG;
	}
	if (header.segment_count == 0 || header.segment_count > CP_MAX_SEGMENTS) {
		ESP_LOGE(TAG, "segment count %u is not plausible", header.segment_count);
		return ESP_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));
	out->segments = header.segment_count;
	out->hash_appended = (header.hash_appended == 1);
	snprintf(out->version, sizeof(out->version), "unknown");

	uint8_t checksum = CP_SEED_CHECKSUM;
	size_t pos = sizeof(esp_image_header_t);

	for (int i = 0; i < header.segment_count; i++) {
		esp_image_segment_header_t seg;
		if (pos + sizeof(seg) > part->size) {
			ESP_LOGE(TAG, "partition ends inside the header of segment %d", i);
			return ESP_ERR_INVALID_SIZE;
		}
		ret = esp_partition_read(part, pos, &seg, sizeof(seg));
		if (ret != ESP_OK) return ret;
		if (seg.data_len > CP_MAX_SEGMENT_LEN) {
			ESP_LOGE(TAG, "segment %d claims %" PRIu32 " bytes - the partition is not an image",
					i, seg.data_len);
			return ESP_ERR_INVALID_SIZE;
		}
		pos += sizeof(seg);
		if (pos + seg.data_len > part->size) {
			ESP_LOGE(TAG, "partition ends inside segment %d", i);
			return ESP_ERR_INVALID_SIZE;
		}

		/* The version string lives at the start of the first segment's data
		 * (esp_app_desc_t), which is why it is read here rather than at a guessed offset. */
		if (i == 0 && seg.data_len >= sizeof(esp_app_desc_t)) {
			esp_app_desc_t desc;
			if (esp_partition_read(part, pos, &desc, sizeof(desc)) == ESP_OK &&
					desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
				snprintf(out->version, sizeof(out->version), "%s", desc.version);
			}
		}

		size_t left = seg.data_len;
		size_t at = pos;
		while (left) {
			const size_t chunk = left > sizeof(s_cp_buf) ? sizeof(s_cp_buf) : left;
			ret = esp_partition_read(part, at, s_cp_buf, chunk);
			if (ret != ESP_OK) return ret;
			for (size_t k = 0; k < chunk; k++) checksum ^= s_cp_buf[k];
			at += chunk;
			left -= chunk;
		}
		pos += seg.data_len;
	}

	/* esptool pads so the checksum byte is the LAST byte of a 16-byte block; the length up
	 * to and including it is therefore a multiple of 16. Nothing is padded before the hash. */
	pos += 15 - (pos % 16);
	if (pos >= part->size) {
		ESP_LOGE(TAG, "partition ends before the image checksum byte");
		return ESP_ERR_INVALID_SIZE;
	}
	uint8_t stored = 0;
	ret = esp_partition_read(part, pos, &stored, 1);
	if (ret != ESP_OK) return ret;
	out->checksum_ok = (stored == checksum);
	pos += 1;

	out->hashed_len = pos;
	out->total_len = pos + (out->hash_appended ? CP_SHA_LEN : 0);
	if (out->total_len > part->size) {
		ESP_LOGE(TAG, "partition ends before the appended SHA-256");
		return ESP_ERR_INVALID_SIZE;
	}
	return ESP_OK;
}

/* Hash the image and compare with the 32 bytes esptool appended to it. */
static esp_err_t cp_image_check_hash(const esp_partition_t* part, const cp_image_t* img)
{
	uint8_t stored[CP_SHA_LEN];
	esp_err_t ret = esp_partition_read(part, img->hashed_len, stored, sizeof(stored));
	if (ret != ESP_OK) return ret;

	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	if (mbedtls_sha256_starts(&ctx, 0) != 0) {
		mbedtls_sha256_free(&ctx);
		return ESP_FAIL;
	}
	size_t at = 0;
	while (at < img->hashed_len) {
		size_t chunk = img->hashed_len - at;
		if (chunk > sizeof(s_cp_buf)) chunk = sizeof(s_cp_buf);
		ret = esp_partition_read(part, at, s_cp_buf, chunk);
		if (ret != ESP_OK) { mbedtls_sha256_free(&ctx); return ret; }
		if (mbedtls_sha256_update(&ctx, s_cp_buf, chunk) != 0) {
			mbedtls_sha256_free(&ctx);
			return ESP_FAIL;
		}
		at += chunk;
	}
	uint8_t computed[CP_SHA_LEN];
	const int fin = mbedtls_sha256_finish(&ctx, computed);
	mbedtls_sha256_free(&ctx);
	if (fin != 0) return ESP_FAIL;

	if (memcmp(stored, computed, CP_SHA_LEN) != 0) {
		ESP_LOGE(TAG, "the image's own SHA-256 does not match its contents");
		ESP_LOGE(TAG, "  stored   %02x%02x%02x%02x...", stored[0], stored[1], stored[2], stored[3]);
		ESP_LOGE(TAG, "  computed %02x%02x%02x%02x...", computed[0], computed[1], computed[2], computed[3]);
		return ESP_ERR_INVALID_CRC;
	}
	return ESP_OK;
}

/* The gate. Nothing is sent to the C6 unless this returns ESP_OK. */
static esp_err_t cp_image_verify(const esp_partition_t* part, cp_image_t* img)
{
	esp_err_t ret = cp_image_scan(part, img);
	if (ret != ESP_OK) return ret;

	ESP_LOGI(TAG, "image: %u bytes, %d segment(s), version %s, hash %s",
			(unsigned int)img->total_len, img->segments, img->version,
			img->hash_appended ? "appended" : "absent");

	if (!img->checksum_ok) {
		ESP_LOGE(TAG, "the image's own checksum byte does not match its segment data");
		return ESP_ERR_INVALID_CRC;
	}
	if (!img->hash_appended) {
		/* Pre-2019 esptool, or an image assembled by hand. The checksum still passed, so
		 * this is a warning and not a refusal - but say so, because it is the weaker check
		 * and someone should know which one the image got. */
		ESP_LOGW(TAG, "no SHA-256 in this image - only the 1-byte checksum could be verified");
		return ESP_OK;
	}
	return cp_image_check_hash(part, img);
}

/* parse_image_header() lived here. It computed only a SIZE, and computed it 16 bytes too
 * large for any image whose segment data does not end on a 16-byte boundary. cp_image_scan()
 * above replaces it: same walk, correct arithmetic, and it verifies while it walks. */

esp_err_t ota_partition_perform(const char* partition_label)
{
	const esp_partition_t* partition;
	esp_err_t ret = ESP_OK;
	uint8_t chunk[CHUNK_SIZE];
	size_t bytes_read;
	size_t offset = 0;
	size_t firmware_size;
	char new_app_version[32];

	ESP_LOGI(TAG, "Starting Partition OTA from partition: %s", partition_label);

	/* Find the partition */
	partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partition_label);
	if (partition == NULL) {
		ESP_LOGE(TAG, "Partition '%s' not found", partition_label);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Found partition: %s, size: %" PRIu32 " bytes", partition->label, partition->size);

	/* Check if partition contains any firmware data */
	ret = check_partition_has_firmware(partition);
	if (ret == ESP_ERR_NOT_FOUND) {
		ESP_LOGW(TAG, "OTA cannot proceed - partition appears to be empty or uninitialized");
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	} else if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to check partition contents: %s", esp_err_to_name(ret));
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	/* player-fork: verify BEFORE anything is sent, and therefore long before anything is
	 * activated. "Firmware verified" used to mean "the header parsed"; now it means the
	 * image's own checksum and SHA-256 agree with its contents. An image that fails here
	 * never reaches esp_hosted_slave_ota_begin(), so main.c never gets to
	 * activate_and_restart() with a broken C6 on the other end - which is the one state
	 * this panel cannot recover from over the air. */
	cp_image_t image;
	ret = cp_image_verify(partition, &image);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "REFUSING to flash the C6: the image in '%s' did not verify (%s)",
				partition->label, esp_err_to_name(ret));
		ESP_LOGE(TAG, "Nothing was sent to the co-processor; it is still running what it had.");
		ESP_LOGE(TAG, "Check the file on the PC first: python tools/c6_image.py verify <bin>");
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}
	firmware_size = image.total_len;
	snprintf(new_app_version, sizeof(new_app_version), "%s", image.version);

	ESP_LOGI(TAG, "Firmware verified - Size: %u bytes, Version: %s", (unsigned int)firmware_size, new_app_version);

#ifdef CONFIG_OTA_VERSION_CHECK_SLAVEFW_SLAVE
	/* Get current running slave firmware version */
	esp_hosted_coprocessor_fwver_t current_slave_version = {0};
	esp_err_t version_ret = esp_hosted_get_coprocessor_fwversion(&current_slave_version);

	if (version_ret == ESP_OK) {
		char current_version_str[32];
		snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
				current_slave_version.major1, current_slave_version.minor1, current_slave_version.patch1);

		ESP_LOGI(TAG, "Current slave firmware version: %s", current_version_str);
		ESP_LOGI(TAG, "New slave firmware version: %s", new_app_version);

		/* player-fork: the CP image's app description carries the git tag ("v3.0.7") while the
		 * co-processor reports "3.0.7"; without stripping the prefix the updater re-flashed an
		 * up-to-date CP on every boot (seen on the bench). */
		const char *new_ver = new_app_version;
		if (*new_ver == 'v' || *new_ver == 'V') {
			new_ver++;
		}
		if (strcmp(new_ver, current_version_str) == 0) {
			ESP_LOGW(TAG, "Current slave firmware version (%s) is the same as new version (%s). Skipping OTA.",
					current_version_str, new_app_version);
			return ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED;
		}

		ESP_LOGI(TAG, "Version differs - proceeding with OTA from %s to %s", current_version_str, new_app_version);
	} else {
		ESP_LOGW(TAG, "Could not get current slave firmware version (error: %s), proceeding with OTA",
				esp_err_to_name(version_ret));
	}
#else
	ESP_LOGI(TAG, "Version check disabled - proceeding with OTA (new firmware version: %s)", new_app_version);
#endif

	/* Validate firmware size */
	if (firmware_size == 0) {
		ESP_LOGE(TAG, "Firmware size is 0, cannot proceed with OTA");
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}
	if (firmware_size > partition->size) {
		ESP_LOGE(TAG, "Firmware size (%u) exceeds partition size (%" PRIu32 ")", (unsigned int)firmware_size, partition->size);
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	ESP_LOGI(TAG, "Proceeding with OTA - Firmware size: %u bytes", (unsigned int)firmware_size);

	/* Begin OTA */
	ret = esp_hosted_slave_ota_begin();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(ret));
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	/* Read firmware from partition in chunks - only up to actual firmware size */
	uint32_t total_bytes_sent = 0;
	uint32_t chunk_count = 0;

	while (offset < firmware_size) {
		bytes_read = (firmware_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (firmware_size - offset);

		ret = esp_partition_read(partition, offset, chunk, bytes_read);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(ret));
			esp_hosted_slave_ota_end();
			return ESP_HOSTED_SLAVE_OTA_FAILED;
		}

		ret = esp_hosted_slave_ota_write(chunk, bytes_read);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to write OTA chunk %" PRIu32 ": %s", chunk_count, esp_err_to_name(ret));
			esp_hosted_slave_ota_end();
			return ESP_HOSTED_SLAVE_OTA_FAILED;
		}

		total_bytes_sent += bytes_read;
		offset += bytes_read;
		chunk_count++;

		/* Progress indicator */
		if (chunk_count % 50 == 0) {
			ESP_LOGD(TAG, "Progress: %" PRIu32 "/%u bytes (%.1f%%)",
					total_bytes_sent, (unsigned int)firmware_size, (float)total_bytes_sent * 100 / firmware_size);
		}
	}

	ESP_LOGD(TAG, "Total chunks sent: %" PRIu32 ", Total bytes sent: %" PRIu32, chunk_count, total_bytes_sent);

	/* End OTA */
	ret = esp_hosted_slave_ota_end();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(ret));
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	ESP_LOGI(TAG, "Partition OTA completed successfully - Sent %u bytes", (unsigned int)firmware_size);
	return ESP_HOSTED_SLAVE_OTA_COMPLETED;
}
