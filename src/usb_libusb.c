// Generated using AI assistant
// Was not reviewed

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usb.h"

#define MAX_RETRIES 0

struct usb_descriptor_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
};

int usb_read(usb_handle *h, void *_data, int len, int timeout) {
    unsigned char *data = (unsigned char *)_data;
    int transferred = 0; // Use int for transferred bytes
    int total_transferred = 0;
    int r = 0;
    int retry;

    if (h->ep_in == 0 || h->handle == NULL) {
        return -1;
    }

    while (len > 0 && r != LIBUSB_ERROR_TIMEOUT) {
        int xfer = len; // libusb handles fragmentation internally

        retry = 0;
        do {
            unsigned char recv_buf[1024]; // Supposed to be max USB bulk size
            //DBG("[ usb read %d ], fname=%s\n", xfer, h->fname); // Remove fname, if it is not used in your libusb version
            r = libusb_bulk_transfer(h->handle, h->ep_in, recv_buf, sizeof(recv_buf), &transferred, timeout < 1 ? 1 : timeout);
            for (int i = 0; i < transferred; i++) {
                cb_push_back(h->circbuf, &recv_buf[i]);
            }
            //DBG("[ usb read %d ] = %d, fname=%s, Retry %d \n", xfer, r, h->fname, retry);
            if (r < 0 && r != LIBUSB_ERROR_TIMEOUT) {
                // fprintf(stderr, "ERROR: libusb_bulk_transfer() failed: %s\n", libusb_strerror(r));
                if (++retry > MAX_RETRIES) {
                    return r;
                }
                sleep(1);
            }
        } while (r < 0 && r != LIBUSB_ERROR_TIMEOUT);

        for (int i = 0; (h->circbuf->count > 0) && (len > 0); i++) {
            cb_pop_front(h->circbuf, data);
            len--;
            data++;
            total_transferred++;
        }
    }

    return total_transferred;
}

int usb_write(usb_handle *h, const void *_data, int len, int timeout) {
    unsigned char *data = (unsigned char *)_data;
    int transferred = 0;
    int total_transferred = 0;
    int r;

    if (h->ep_out == 0 || h->handle == NULL) {
        return -1;
    }

    while (len > 0) {
        int xfer = len; // libusb handles fragmentation

        r = libusb_bulk_transfer(h->handle, h->ep_out, data, xfer, &transferred, timeout < 1 ? 1 : timeout);
        if (r < 0 || transferred != xfer) { // Check if the transfer was successful and all bytes were sent.
            fprintf(stderr, "WARNING: libusb_bulk_transfer() failed: %s (transferred %d of %d)\n", libusb_strerror(r), transferred, xfer);
            if (r == LIBUSB_ERROR_TIMEOUT) {
                // Do not treat as an error
                return total_transferred;
            }
            return -1;
        }

        len -= transferred; // Update the remaining length
        data += transferred; // Move the data pointer
        total_transferred += transferred;
    }

    return total_transferred;
}

int usb_close(usb_handle *h) {
    if (h) {
        if (h->circbuf) {
            cb_free(h->circbuf);
        }

        if (h->handle) { // Check if handle is valid
            int r = libusb_release_interface(h->handle, h->interface_number); // Release the claimed interface
            if(r < 0) {
                fprintf(stderr, "libusb_release_interface() failed: %s\n", libusb_strerror(r));
            }

            libusb_close(h->handle);
            h->handle = NULL; // Mark handle as invalid
        }

        libusb_exit(h->context);
        h->context = NULL; // Mark context as invalid

        //DBG("[ usb closed ]\n"); // No fd to print anymore

        // Free the usb_handle structure if it was dynamically allocated.
        // if you allocated usb_handle with malloc, you should free it here:
        free(h);
        h = NULL; // Good practice to prevent dangling pointers
    }

    return 0;
}

static int filter_usb_device(libusb_device *dev, const struct libusb_device_descriptor *dev_desc, int writable,
                             ifc_match_func callback, int *ept_in_id, int *ept_out_id, int *ifc_id) {

    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r < 0) {
        fprintf(stderr, "libusb_get_active_config_descriptor() failed: %s\n", libusb_strerror(r));
        return -1;
    }

    struct usb_ifc_info info;
    info.dev_vendor = dev_desc->idVendor;
    info.dev_product = dev_desc->idProduct;
    info.dev_class = dev_desc->bDeviceClass;
    info.dev_subclass = dev_desc->bDeviceSubClass;
    info.dev_protocol = dev_desc->bDeviceProtocol;
    info.writable = writable; // You'll need to determine writability with libusb

    // Get serial number (libusb version)
    info.serial_number[0] = '\0';
    if (dev_desc->iSerialNumber) {
        libusb_device_handle *handle = NULL; // Declare a handle
        int r = libusb_open(dev, &handle); // Open the device
        if (r == 0) { // Check if open was successful
            unsigned char serial_buffer[256];
            r = libusb_get_string_descriptor_ascii(handle, dev_desc->iSerialNumber, serial_buffer, sizeof(serial_buffer)); // Use the handle
            if (r > 0) {
                strncpy(info.serial_number, (char *)serial_buffer, sizeof(info.serial_number) - 1);
                info.serial_number[sizeof(info.serial_number) - 1] = '\0';
            }
            libusb_close(handle); // Close the handle
        } else {
            // fprintf(stderr, "libusb_open() failed for %04x:%04x: %s\n", info.dev_vendor, info.dev_product, libusb_strerror(r));
        }
    }

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *ifc = &cfg->interface[i];
        for (int j = 0; j < ifc->num_altsetting; j++) {
            const struct libusb_interface_descriptor *ifc_desc = &ifc->altsetting[j];

            int in = -1;
            int out = -1;
            info.ifc_class = ifc_desc->bInterfaceClass;
            info.ifc_subclass = ifc_desc->bInterfaceSubClass;
            info.ifc_protocol = ifc_desc->bInterfaceProtocol;

            for (int k = 0; k < ifc_desc->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep_desc = &ifc_desc->endpoint[k];

                if ((ep_desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }

                if (ep_desc->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in = ep_desc->bEndpointAddress;
                } else {
                    out = ep_desc->bEndpointAddress;
                }
            }

            info.has_bulk_in = (in != -1);
            info.has_bulk_out = (out != -1);

            snprintf(info.device_path, sizeof(info.device_path), "usb:%04x:%04x", dev_desc->idVendor, dev_desc->idProduct); //More general device path

            if (callback(&info) == 0) {
                *ept_in_id = in;
                *ept_out_id = out;
                *ifc_id = ifc_desc->bInterfaceNumber;
                libusb_free_config_descriptor(cfg);
                return 0;
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    return -1;
}

static usb_handle *find_usb_device(libusb_context *context, ifc_match_func callback) {
    usb_handle *usb = NULL;
    libusb_device **devs;
    int cnt = libusb_get_device_list(context, &devs);
    if (cnt < 0) {
        fprintf(stderr, "libusb_get_device_list() failed: %s\n", libusb_strerror(cnt));
        return NULL;
    }

    for (int i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        int r = libusb_get_device_descriptor(devs[i], &desc);
        if (r < 0) {
            fprintf(stderr, "libusb_get_device_descriptor() failed: %s\n", libusb_strerror(r));
            continue; // Skip this device
        }

        int in, out, ifc;
        int writable = 1; // You might need a way to determine writability with libusb

        if (filter_usb_device(devs[i], &desc, writable, callback, &in, &out, &ifc) == 0) {
            usb = calloc(1, sizeof(usb_handle));
            if (!usb) {
                fprintf(stderr, "Memory allocation failed\n");
                break; // Exit the loop
            }

            r = libusb_open(devs[i], &usb->handle);
            if (r < 0) {
                fprintf(stderr, "libusb_open() failed: %s\n", libusb_strerror(r));
                free(usb);
                usb = NULL;
                continue; // Try the next device
            }
            
            r = libusb_claim_interface(usb->handle, ifc); // Claim the interface
            usb->interface_number = ifc;
            if (r < 0) {
                fprintf(stderr, "libusb_claim_interface() failed: %s\n", libusb_strerror(r));
                libusb_close(usb->handle);
                free(usb);
                usb = NULL;
                continue; // Try the next device
            }

            usb->ep_in = in;
            usb->ep_out = out;
            // usb->fname is not really needed in libusb approach

            usb->context = context;

            break; // Device found and opened
        }
    }

    libusb_free_device_list(devs, 1); // Free the list

    return usb;
}

usb_handle *usb_open(ifc_match_func callback)
{
    libusb_context *context = NULL;
    int r = libusb_init(&context);
    if (r < 0) {
        fprintf(stderr, "libusb_init() failed: %s\n", libusb_strerror(r));
        return 0;
    }

    usb_handle *usb_dev = find_usb_device(context, callback);

    if (usb_dev) {
        // Use the usb_dev handle (e.g., perform transfers)
        usb_dev->circbuf = calloc(1, sizeof(circular_buffer));
        cb_init(usb_dev->circbuf, MAX_TRANSFER_SIZE, sizeof(unsigned char));
        printf("Device found!\n");
    } else {
        printf("Device not found.\n");
    }

    return usb_dev;
}
