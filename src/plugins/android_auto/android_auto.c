#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <alloca.h>
#include <time.h>

#include <openssl/ssl.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/conf.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <plugins/android_auto/android_auto.h>
#include <aasdk/ControlMessageIdsEnum.pb-c.h>

static const char *certificate_string = "-----BEGIN CERTIFICATE-----\n\
MIIDKjCCAhICARswDQYJKoZIhvcNAQELBQAwWzELMAkGA1UEBhMCVVMxEzARBgNV\n\
BAgMCkNhbGlmb3JuaWExFjAUBgNVBAcMDU1vdW50YWluIFZpZXcxHzAdBgNVBAoM\n\
Fkdvb2dsZSBBdXRvbW90aXZlIExpbmswJhcRMTQwNzA0MDAwMDAwLTA3MDAXETQ1\n\
MDQyOTE0MjgzOC0wNzAwMFMxCzAJBgNVBAYTAkpQMQ4wDAYDVQQIDAVUb2t5bzER\n\
MA8GA1UEBwwISGFjaGlvamkxFDASBgNVBAoMC0pWQyBLZW53b29kMQswCQYDVQQL\n\
DAIwMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAM911mNnUfx+WJtx\n\
uk06GO7kXRW/gXUVNQBkbAFZmVdVNvLoEQNthi2X8WCOwX6n6oMPxU2MGJnvicP3\n\
6kBqfHhfQ2Fvqlf7YjjhgBHh0lqKShVPxIvdatBjVQ76aym5H3GpkigLGkmeyiVo\n\
VO8oc3cJ1bO96wFRmk7kJbYcEjQyakODPDu4QgWUTwp1Z8Dn41ARMG5OFh6otITL\n\
XBzj9REkUPkxfS03dBXGr5/LIqvSsnxib1hJ47xnYJXROUsBy3e6T+fYZEEzZa7y\n\
7tFioHIQ8G/TziPmvFzmQpaWMGiYfoIgX8WoR3GD1diYW+wBaZTW+4SFUZJmRKgq\n\
TbMNFkMCAwEAATANBgkqhkiG9w0BAQsFAAOCAQEAsGdH5VFn78WsBElMXaMziqFC\n\
zmilkvr85/QpGCIztI0FdF6xyMBJk/gYs2thwvF+tCCpXoO8mjgJuvJZlwr6fHzK\n\
Ox5hNUb06AeMtsUzUfFjSZXKrSR+XmclVd+Z6/ie33VhGePOPTKYmJ/PPfTT9wvT\n\
93qswcxhA+oX5yqLbU3uDPF1ZnJaEeD/YN45K/4eEA4/0SDXaWW14OScdS2LV0Bc\n\
YmsbkPVNYZn37FlY7e2Z4FUphh0A7yME2Eh/e57QxWrJ1wubdzGnX8mrABc67ADU\n\
U5r9tlTRqMs7FGOk6QS2Cxp4pqeVQsrPts4OEwyPUyb3LfFNo3+sP111D9zEow==\n\
-----END CERTIFICATE-----\n";

static const char *private_key_string = "-----BEGIN RSA PRIVATE KEY-----\n\
MIIEowIBAAKCAQEAz3XWY2dR/H5Ym3G6TToY7uRdFb+BdRU1AGRsAVmZV1U28ugR\n\
A22GLZfxYI7Bfqfqgw/FTYwYme+Jw/fqQGp8eF9DYW+qV/tiOOGAEeHSWopKFU/E\n\
i91q0GNVDvprKbkfcamSKAsaSZ7KJWhU7yhzdwnVs73rAVGaTuQlthwSNDJqQ4M8\n\
O7hCBZRPCnVnwOfjUBEwbk4WHqi0hMtcHOP1ESRQ+TF9LTd0Fcavn8siq9KyfGJv\n\
WEnjvGdgldE5SwHLd7pP59hkQTNlrvLu0WKgchDwb9POI+a8XOZClpYwaJh+giBf\n\
xahHcYPV2Jhb7AFplNb7hIVRkmZEqCpNsw0WQwIDAQABAoIBAB2u7ZLheKCY71Km\n\
bhKYqnKb6BmxgfNfqmq4858p07/kKG2O+Mg1xooFgHrhUhwuKGbCPee/kNGNrXeF\n\
pFW9JrwOXVS2pnfaNw6ObUWhuvhLaxgrhqLAdoUEgWoYOHcKzs3zhj8Gf6di+edq\n\
SyTA8+xnUtVZ6iMRKvP4vtCUqaIgBnXdmQbGINP+/4Qhb5R7XzMt/xPe6uMyAIyC\n\
y5Fm9HnvekaepaeFEf3bh4NV1iN/R8px6cFc6ELYxIZc/4Xbm91WGqSdB0iSriaZ\n\
TjgrmaFjSO40tkCaxI9N6DGzJpmpnMn07ifhl2VjnGOYwtyuh6MKEnyLqTrTg9x0\n\
i3mMwskCgYEA9IyljPRerXxHUAJt+cKOayuXyNt80q9PIcGbyRNvn7qIY6tr5ut+\n\
ZbaFgfgHdSJ/4nICRq02HpeDJ8oj9BmhTAhcX6c1irH5ICjRlt40qbPwemIcpybt\n\
mb+DoNYbI8O4dUNGH9IPfGK8dRpOok2m+ftfk94GmykWbZF5CnOKIp8CgYEA2Syc\n\
5xlKB5Qk2ZkwXIzxbzozSfunHhWWdg4lAbyInwa6Y5GB35UNdNWI8TAKZsN2fKvX\n\
RFgCjbPreUbREJaM3oZ92o5X4nFxgjvAE1tyRqcPVbdKbYZgtcqqJX06sW/g3r/3\n\
RH0XPj2SgJIHew9sMzjGWDViMHXLmntI8rVA7d0CgYBOr36JFwvrqERN0ypNpbMr\n\
epBRGYZVSAEfLGuSzEUrUNqXr019tKIr2gmlIwhLQTmCxApFcXArcbbKs7jTzvde\n\
PoZyZJvOr6soFNozP/YT8Ijc5/quMdFbmgqhUqLS5CPS3z2N+YnwDNj0mO1aPcAP\n\
STmcm2DmxdaolJksqrZ0owKBgQCD0KJDWoQmaXKcaHCEHEAGhMrQot/iULQMX7Vy\n\
gl5iN5E2EgFEFZIfUeRWkBQgH49xSFPWdZzHKWdJKwSGDvrdrcABwdfx520/4MhK\n\
d3y7CXczTZbtN1zHuoTfUE0pmYBhcx7AATT0YCblxrynosrHpDQvIefBBh5YW3AB\n\
cKZCOQKBgEM/ixzI/OVSZ0Py2g+XV8+uGQyC5XjQ6cxkVTX3Gs0ZXbemgUOnX8co\n\
eCXS4VrhEf4/HYMWP7GB5MFUOEVtlLiLM05ruUL7CrphdfgayDXVcTPfk75lLhmu\n\
KAwp3tIHPoJOQiKNQ3/qks5km/9dujUGU2ARiU3qmxLMdgegFz8e\n\
-----END RSA PRIVATE KEY-----\n";

static struct aaplugin aaplugin;

int get_errno_for_libusb_error(int libusb_error) {
    static int errors[LIBUSB_ERROR_COUNT - 1] = {
        [LIBUSB_SUCCESS] = 0,
        [-LIBUSB_ERROR_IO] = EIO,
        [-LIBUSB_ERROR_INVALID_PARAM] = EINVAL,
        [-LIBUSB_ERROR_ACCESS] = EACCES,
        [-LIBUSB_ERROR_NO_DEVICE] = ENODEV,
        [-LIBUSB_ERROR_NOT_FOUND] = ENOENT,
        [-LIBUSB_ERROR_BUSY] = EBUSY,
        [-LIBUSB_ERROR_TIMEOUT] = ETIMEDOUT,
        [-LIBUSB_ERROR_OVERFLOW] = EOVERFLOW,
        [-LIBUSB_ERROR_PIPE] = EPIPE,
        [-LIBUSB_ERROR_INTERRUPTED] = EINTR,
        [-LIBUSB_ERROR_NO_MEM] = ENOMEM,
        [-LIBUSB_ERROR_NOT_SUPPORTED] = ENOTSUP
    };

    static int transfer_statuses[7] = {
        [LIBUSB_TRANSFER_COMPLETED] = 0,
        [LIBUSB_TRANSFER_ERROR] = EIO,
        [LIBUSB_TRANSFER_TIMED_OUT] = ETIMEDOUT,
        [LIBUSB_TRANSFER_CANCELLED] = ECANCELED,
        [LIBUSB_TRANSFER_STALL] = ECOMM,
        [LIBUSB_TRANSFER_NO_DEVICE] = ENODEV,
        [LIBUSB_TRANSFER_OVERFLOW] = EOVERFLOW
    };

    if (libusb_error < 0) {
        int index = -libusb_error;
        if (index < (int) (sizeof(errors) / sizeof(*errors))) {
            return errors[index];
        } else {
            return EINVAL;
        }
    } else if (libusb_error == 0) {
        return 0;
    } else {
        if (libusb_error < (int) (sizeof(transfer_statuses) / sizeof(*transfer_statuses))) {
            return transfer_statuses[libusb_error];
        } else {
            return EINVAL;
        }
    }
}

const char *get_str_for_libusb_error(int libusb_error) {
    if (libusb_error <= 0) {
        return libusb_strerror(libusb_error);
    } else {
        return libusb_error_name(libusb_error);
    }
}


static void on_control_transfer_completed(struct libusb_transfer *transfer) {
	bool *completed;
    completed = transfer->user_data;
    *completed = true;
}

int libusb_control_transfer_mt_(
    struct libusb_context *ctx,
    libusb_device_handle *dev_handle,
	uint8_t bmRequestType,
    uint8_t bRequest,
    uint16_t wValue,
    uint16_t wIndex,
	unsigned char *data,
    uint16_t wLength,
    unsigned int timeout
) {
	struct libusb_transfer *transfer;
	unsigned char *buffer;
	bool completed = false;
	int ok;

	transfer = libusb_alloc_transfer(0);
	if (transfer == NULL) {
        return LIBUSB_ERROR_NO_MEM;
    }

	buffer = alloca(LIBUSB_CONTROL_SETUP_SIZE + wLength);

	libusb_fill_control_setup(
        buffer,
        bmRequestType,
        bRequest,
        wValue,
        wIndex,
		wLength
    );
	if ((bmRequestType & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
		memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, data, wLength);
    }

	libusb_fill_control_transfer(
        transfer,
        dev_handle,
        buffer,
        on_control_transfer_completed,
        &completed,
        timeout
    );

    libusb_lock_event_waiters(ctx);

	ok = libusb_submit_transfer(transfer);
	if (ok < 0) {
		libusb_free_transfer(transfer);
		return ok;
	}

    while (completed == false) {
        ok = libusb_wait_for_event(ctx, NULL);
        if (ok == 1) {
            libusb_unlock_event_waiters(ctx);
            libusb_free_transfer(transfer);
            fprintf(stderr, "[android-auto plugin] USB control transfer timed out.\n");
            return LIBUSB_ERROR_TIMEOUT;
        }
    }
    libusb_unlock_event_waiters(ctx);

	if ((bmRequestType & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
		memcpy(
            data,
            libusb_control_transfer_get_data(transfer),
			transfer->actual_length
        );
    }

	switch (transfer->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		ok = transfer->actual_length;
		break;
	case LIBUSB_TRANSFER_TIMED_OUT:
		ok = LIBUSB_ERROR_TIMEOUT;
		break;
	case LIBUSB_TRANSFER_STALL:
		ok = LIBUSB_ERROR_PIPE;
		break;
	case LIBUSB_TRANSFER_NO_DEVICE:
		ok = LIBUSB_ERROR_NO_DEVICE;
		break;
	case LIBUSB_TRANSFER_OVERFLOW:
		ok = LIBUSB_ERROR_OVERFLOW;
		break;
	case LIBUSB_TRANSFER_ERROR:
	case LIBUSB_TRANSFER_CANCELLED:
		ok = LIBUSB_ERROR_IO;
		break;
	default:
		ok = LIBUSB_ERROR_OTHER;
        break;
	}

	libusb_free_transfer(transfer);
	return ok;
}


static int init_ssl(struct aaplugin *aaplugin) {
    const SSL_METHOD *method;
    const BIO_METHOD *read_bio;
    const BIO_METHOD *write_bio;
    EVP_PKEY *private_key;
    SSL_CTX *context;
    X509 *certificate;
    SSL *ssl;
    BIO *bio;
    int ok;

    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();

    bio = BIO_new_mem_buf(certificate_string, -1);
    if (bio == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not create OpenSSL BIO from certificate\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    certificate = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
    if (certificate == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not read OpenSSL X509 certificate\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    BIO_free(bio);

    bio = BIO_new_mem_buf(private_key_string, -1);
    if (bio == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not create OpenSSL BIO from private key\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    private_key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    if (private_key == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not read OpenSSL private key\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    BIO_free(bio);

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    method = TLSv1_2_client_method();
#else
    method = TLS_client_method();
#endif

    context = SSL_CTX_new(method);
    if (context == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not create OpenSSL context\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    ok = SSL_CTX_use_certificate(context, certificate);
    if (!ok) {
        fprintf(stderr, "[android-auto plugin] Could not configure OpenSSL context to use X509 certificate\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    ok = SSL_CTX_use_PrivateKey(context, private_key);
    if (!ok) {
        fprintf(stderr, "[android-auto plugin] Could not configure OpenSSL context to use private key\n");
        ERR_print_errors_fp(stderr);
        return EINVAL;
    }

    aaplugin->ssl_context = context;

    return 0;
}

static int on_libusb_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    static const struct timeval zerotv = {.tv_sec = 0, .tv_usec = 0};
    struct aaplugin *aaplugin;
    int ok;

    (void) revents;

    aaplugin = userdata;

    ok = libusb_handle_events_locked(aaplugin->libusb_context, (struct timeval*) &zerotv);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Error while handling libusb events. libusb_handle_events_locked: %s\n", get_str_for_libusb_error(ok));
        return get_errno_for_libusb_error(ok);
    }

    //libusb_lock_event_waiters(aaplugin->libusb_context);
    libusb_unlock_events(aaplugin->libusb_context);
    //libusb_unlock_event_waiters(aaplugin->libusb_context);
    libusb_lock_events(aaplugin->libusb_context);

    return 0;
}

static void on_libusb_fd_added(int fd, short events, void *user_data) {
    struct aaplugin *aaplugin;
    int ok;

    aaplugin = user_data;

    flutterpi_sd_event_add_io(
        NULL,
        fd,
        events,
        on_libusb_fd_ready,
        aaplugin
    );
}

static int init_usb(struct aaplugin *aaplugin) {
    const struct libusb_pollfd **pollfds;
    struct libusb_context *ctx;
    int ok;

    ok = libusb_init(&ctx);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Could not initialize libusb. libusb_init: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_return_ok;
    }

    ok = libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Could not enable libusb logging. libusb_set_option: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_exit_libusb;
    }

    libusb_set_pollfd_notifiers(
        aaplugin->libusb_context,
        on_libusb_fd_added,
        NULL /* we don't need to be notified on fd_removed, because the fd will be closed anyway */,
        aaplugin
    );

    pollfds = libusb_get_pollfds(ctx);
    if (pollfds == NULL) {
        fprintf(stderr, "[android-auto plugin] Could not integrate libusb with flutter-pi event loop. libusb_get_pollfds\n");
        ok = EINVAL;
        goto fail_exit_libusb;
    }

    for (const struct libusb_pollfd **cursor = pollfds; *cursor; cursor++) {
        ok = flutterpi_sd_event_add_io(
            NULL,
            (*cursor)->fd,
            (*cursor)->events,
            on_libusb_fd_ready,
            aaplugin
        );
        if (ok != 0) {
            goto fail_exit_libusb;
        }
    }

    aaplugin->libusb_context = ctx;

    return 0;


    fail_exit_libusb:
    libusb_exit(ctx);

    fail_return_ok:
    return ok;
}

static void deinit_usb(struct aaplugin *aaplugin) {
    libusb_exit(aaplugin->libusb_context);
    aaplugin->libusb_context = NULL;
    aaplugin->hotplug_cb_handle = 0;
}


static int send_string(
    struct libusb_context *context,
    libusb_device_handle *handle,
    enum accessory_string id,
    const char *string
) {
    size_t string_len = strlen(string);
    uint8_t buffer[string_len + 1];

    (void) context;

    strcpy((char*) buffer, string);

    return libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        ACCESSORY_SEND_STRING,
        0,
        id,
        buffer,
        string_len + 1,
        TRANSFER_TIMEOUT_MS
    );
}


static void *aoa_switcher_entry(void *arg) {
    libusb_device_handle *handle;
    libusb_context *context;
    libusb_device *dev;
    uint16_t version;
    int ok;

    context = ((struct aoa_switcher_args*) arg)->context;
    dev = ((struct aoa_switcher_args*) arg)->device;

    free(arg);

    ok = libusb_open(dev, &handle);
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not get open USB device. libusb_open: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        return (void*) ok;
    }

    ok = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        ACCESSORY_GET_PROTOCOL,
        0,
        0,
        (uint8_t*) &version,
        2,
        TRANSFER_TIMEOUT_MS
    );
    if ((ok < 0) && (ok != LIBUSB_ERROR_PIPE)) {
        fprintf(stderr, "[android-auto plugin] Could not get Android Open Accessory protocol version. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    } else if ((ok < 0) && (ok != LIBUSB_ERROR_PIPE)) {
        ok = 0;
        goto fail_close_handle;
    }

    version = libusb_le16_to_cpu(version);
    if (version == 0) {
        fprintf(stderr, "[android-auto plugin] Android Open Accessory-capable device returned invalid protocol version.\n");
        ok = 0;
        goto fail_close_handle;
    }
    
    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_MANUFACTURER,
        "Android"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send manufacturer string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_MODEL,
        "Android Auto"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send model string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_DESCRIPTION,
        "Android Auto"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send description string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_VERSION,
        "2.0.1"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send version string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_URI,
        "https://github.com/ardera/flutter-pi"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send URI string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = send_string(
        context,
        handle,
        ACCESSORY_STRING_SERIAL,
        "HU-AAAAAA001"
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not send serial string to Android Open Accessory-capable device. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    ok = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        ACCESSORY_START,
        0,
        0,
        NULL,
        0,
        TRANSFER_TIMEOUT_MS
    );
    if (ok < 0) {
        fprintf(stderr, "[android-auto plugin] Could not start USB device accessory mode. libusb_control_transfer: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    libusb_close(handle);

    return (void*) 0;


    fail_close_handle:
    libusb_close(handle);

    fail_free_aoa_device:
    free(dev);

    fail_return_ok:
    return (void*) ok;
}

static void *aoa_dev_mgr_entry(void *arg) {
    const struct libusb_interface_descriptor *face_desc;
    struct libusb_config_descriptor *config;
    const struct libusb_interface *face;
    libusb_device_handle *usb_handle;
    struct aa_device *aadev;
    struct aaplugin *aaplugin;
    libusb_device *usbdev;
    BIO *rbio, *wbio;
    SSL *ssl;
    int ok;

    aaplugin = ((struct aoa_device*) arg)->aaplugin;
    usbdev = ((struct aoa_device*) arg)->device;
    free(arg);

    ok = libusb_open(usbdev, &usb_handle);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Couldn't open AOA device. libusb_open: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_return_ok;
    }

    ok = libusb_get_config_descriptor(usbdev, 0, &config);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Couldn't get config descriptor for AOA device. libusb_get_config_descriptor: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_close_handle;
    }

    if (config->bNumInterfaces == 0) {
        fprintf(stderr, "[android-auto plugin] AOA USB device has no interfaces.\n");
        ok = EINVAL;
        goto fail_free_config_descriptor;
    }

    face = config->interface + 0;

    if (face->num_altsetting == 0) {
        fprintf(stderr, "[android-auto plugin] AOA USB device has no altsettings for interface[0].\n");
        ok = EINVAL;
        goto fail_free_config_descriptor;
    }

    face_desc = face->altsetting + 0;

    if (face_desc->bNumEndpoints < 2) {
        fprintf(stderr, "[android-auto plugin] Couldn't obtain AOA device USB endpoints.\n");
        ok = EINVAL;
        goto fail_free_config_descriptor;
    }

    ok = libusb_claim_interface(usb_handle, face_desc->bInterfaceNumber);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Couldn't claim USB device interface. libusb_claim_interface: %s\n", get_str_for_libusb_error(ok));
        ok = get_errno_for_libusb_error(ok);
        goto fail_free_config_descriptor;
    }

    aadev = malloc(sizeof *aadev);
    if (aadev == NULL) {
        ok = ENOMEM;
        goto fail_free_config_descriptor;
    }

    aadev->aaplugin = aaplugin;
    aadev->connection = kUSB;
    aadev->usb_device = usbdev;
    aadev->usb_handle = usb_handle;
    aadev->receive_buffer_info[0].length = 0;
    aadev->receive_buffer_info[0].start = 0;
    aadev->receive_buffer_info[1].length = 0;
    aadev->receive_buffer_info[1].start = 0;
    aadev->receive_buffer_index = 0;
    aadev->device_name = NULL;
    aadev->device_brand = NULL;
    aadev->channels = PSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE);

    if ((face_desc->endpoint[0].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
        aadev->in_endpoint = face_desc->endpoint[0];
        aadev->out_endpoint = face_desc->endpoint[1];
    } else {
        aadev->in_endpoint = face_desc->endpoint[1];
        aadev->out_endpoint = face_desc->endpoint[0];
    }

    libusb_free_config_descriptor(config);

    ok = aa_xfer_buffer_initialize_on_stack_for_device(aadev->receive_buffers + 0, aadev, aadev->in_endpoint.wMaxPacketSize);
    if (ok != 0) {
        goto fail_free_aadev;
    }

    ok = aa_xfer_buffer_initialize_on_stack_for_device(aadev->receive_buffers + 1, aadev, aadev->in_endpoint.wMaxPacketSize);
    if (ok != 0) {
        goto fail_free_receive_buffer_0;
    }

    ssl = SSL_new(aaplugin->ssl_context);
    if (ssl == NULL) {
        ERR_print_errors_fp(stderr);
        ok = EINVAL;
        goto fail_free_receive_buffer_1;
    }
    aadev->ssl = ssl;

    /*
    BIO_METHOD *meth = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "android-auto transport");

    BIO_meth_set_write_ex(meth, aa_transport_bio_write);
    BIO_meth_set_read_ex(meth, aa_transport_bio_read);
    BIO_meth_set_puts(meth, aa_transport_bio_puts);
    BIO_meth_set_gets(meth, aa_transport_bio_gets);
    BIO_meth_set_ctrl(meth, aa_transport_bio_ctrl);
    BIO_meth_set_create(meth, aa_transport_bio_create);
    BIO_meth_set_destroy(meth, aa_transport_bio_destroy);
    */

    rbio = BIO_new(BIO_s_mem());
    if (rbio == NULL) {
        ERR_print_errors_fp(stderr);
        ok = ENOMEM;
        goto fail_free_ssl;
    }

    wbio = BIO_new(BIO_s_mem());
    if (wbio == NULL) {
        ERR_print_errors_fp(stderr);
        ok = ENOMEM;
        goto fail_free_rbio;
    }

    //BIO_set_callback(rbio, BIO_debug_callback);
    //BIO_set_callback(wbio, BIO_debug_callback);

    SSL_set_bio(ssl, rbio, wbio);

    SSL_set_connect_state(ssl);
    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

    aaplugin->aa_device = aadev;
    ok = aa_dev_manage(aadev);

    SSL_free(ssl);
    aa_xfer_buffer_free(aadev->receive_buffers + 0);
    aa_xfer_buffer_free(aadev->receive_buffers + 1);
    libusb_close(usb_handle);
    free(aadev);

    return (void*) ok;


    fail_free_rbio:
    BIO_free(rbio);

    fail_free_ssl:
    SSL_free(ssl);

    fail_free_receive_buffer_1:
    aa_xfer_buffer_free(aadev->receive_buffers + 1);

    fail_free_receive_buffer_0:
    aa_xfer_buffer_free(aadev->receive_buffers + 0);

    fail_free_aadev:
    free(aadev);
    goto fail_close_handle;

    fail_free_config_descriptor:
    libusb_free_config_descriptor(config);

    fail_close_handle:
    libusb_close(usb_handle);

    fail_free_aa_device:
    free(aadev);

    fail_return_ok:
    return (void*) ok;
}

static int on_aoa_device_arrival(struct aaplugin *aaplugin, libusb_device *usbdev) {
    struct aoa_device *aoadev;
    pthread_t thread;
    int ok;

    printf("[android-auto plugin] An Android Open Accessory device was plugged in.\n");

    aoadev = malloc(sizeof *aoadev);
    if (aoadev == NULL) {
        return ENOMEM;
    }

    aoadev->aaplugin = aaplugin;
    aoadev->device = usbdev;

    ok = pthread_create(&thread, NULL, aoa_dev_mgr_entry, aoadev);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Could not start Android Open Accessory device manager thread. pthread_create: %s\n", strerror(ok));
        free(aoadev);
        return ok;
    }

    pthread_setname_np(thread, "aoa-dev-mgr");

    return 0;
}

static int on_non_aoa_device_arrival(struct aaplugin *aaplugin, libusb_device *device) {
    struct aoa_switcher_args *args;
    pthread_t thread;
    int ok;

    args = malloc(sizeof *args);
    if (args == NULL) {
        return 0;
    }

    args->context = aaplugin->libusb_context;
    args->device = device;

    ok = pthread_create(
        &thread,
        NULL,
        aoa_switcher_entry,
        args
    );
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Couldn't start Android Open Accessory switcher thread. pthread_create: %s\n", strerror(ok));
        free(args);
        return ok;
    }

    pthread_setname_np(thread, "aoa-switcher");

    return 0;
}

static int on_libusb_device_arrived(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data) {
    struct libusb_device_descriptor descriptor;
    struct libusb_device_handle *handle;
    struct aoa_device *dev;
    struct aaplugin *aaplugin;
    int ok;

    (void) ctx;
    (void) event;

    aaplugin = user_data;

    ok = libusb_get_device_descriptor(device, &descriptor);    
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Could not get usb device descriptor. libusb_get_device_descriptor: %s\n", get_str_for_libusb_error(ok));
        return 0;
    }
    
    if (descriptor.idVendor == GOOGLE_VENDOR_ID && (
        descriptor.idProduct == AOAP_PRODUCT_ID ||
        descriptor.idProduct == AOAP_WITH_ADB_PRODUCT_ID
    )) {
        return on_aoa_device_arrival(aaplugin, device) == 0;
    } else {
        on_non_aoa_device_arrival(aaplugin, device);
        return 0;
    }

    return 0;
}


static int enable_usb(struct aaplugin *aaplugin) {
    int ok;

    ok = libusb_hotplug_register_callback(
        aaplugin->libusb_context,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
        LIBUSB_HOTPLUG_NO_FLAGS,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        on_libusb_device_arrived,
        aaplugin,
        &aaplugin->hotplug_cb_handle
    );
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] Could not register libusb hotplug callback. libusb_hotplug_register_callback: %s\n", get_str_for_libusb_error(ok));
        return get_errno_for_libusb_error(ok);
    }

    return 0;
}

static int disable_usb(struct aaplugin *aaplugin) {
    libusb_hotplug_deregister_callback(
        aaplugin->libusb_context,
        aaplugin->hotplug_cb_handle
    );
    return 0;
}

static int enable_bluetooth(struct aaplugin *aaplugin) {
    (void) aaplugin;

    return ENOTSUP;
}

static int disable_bluetooth(struct aaplugin *aaplugin) {
    (void) aaplugin;

    return ENOTSUP;
}

static int enable_wifi(struct aaplugin *aaplugin) {
    (void) aaplugin;

    return ENOTSUP;
}

static int disable_wifi(struct aaplugin *aaplugin) {
    (void) aaplugin;

    return ENOTSUP;
}

int send_android_auto_state(
    struct aaplugin *plugin,
    bool connected,
    bool has_interface,
    enum aa_device_connection interface,
    char *device_name,
    char *device_brand,
    bool has_texture_id,
    int64_t texture_id,
    bool has_is_focused,
    bool is_focused
) {
    (void) plugin;

    char *interface_str = interface == kUSB ? "AndroidAutoInterface.usb" :
        interface == kBluetooth ? "AndroidAutoInterface.bluetooth" :
        interface == kWifi ? "AndroidAutoInterface.wifi" :
        NULL;
    
    if (interface_str == NULL) {
        return EINVAL;
    }

    return platch_send_success_event_std(
        ANDROID_AUTO_EVENT_CHANNEL,
        &STDMAP6(
            STDSTRING("connected"), STDBOOL(connected),
            STDSTRING("interface"), has_interface? STDSTRING(interface_str) : STDNULL,
            STDSTRING("deviceName"), device_name? STDSTRING(device_name) : STDNULL,
            STDSTRING("deviceBrand"), device_brand? STDSTRING(device_brand) : STDNULL,
            STDSTRING("textureId"), has_texture_id? STDINT64(texture_id) : STDNULL,
            STDSTRING("isFocused"), has_is_focused? STDBOOL(is_focused) : STDNULL
        )
    );
}

int sync_android_auto_state(
    struct aaplugin *plugin
) {
    int ok;

    if (plugin->aa_device != NULL) {
        ok = send_android_auto_state(
            plugin,
            true,
            true,
            plugin->aa_device->connection,
            plugin->aa_device->device_name,
            plugin->aa_device->device_brand,
            plugin->aa_device->has_texture_id,
            plugin->aa_device->texture_id,
            true,
            plugin->aa_device->is_focused
        );
    } else {
        ok = send_android_auto_state(
            plugin,
            false,
            false,
            kUSB,
            NULL,
            NULL,
            false,
            -1,
            false,
            false
        );
    }

    return ok;
}

static int on_set_enabled_interfaces(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    bool _enable_usb = false;
    bool _enable_bluetooth = false;
    bool _enable_wifi = false;
    int ok;

    if (STDVALUE_IS_LIST(*arg)) {
        for (size_t i = 0; i < arg->size; i++) {
            if (STDVALUE_IS_STRING(arg->list[i]) == false) {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg` to be a list of string-ifications of the AndroidAutoInterface enum."
                );
            }
        }
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a list of string-ifications of the AndroidAutoInterface enum."
        );
    }

    for (size_t i = 0; i < arg->size; i++) {
        if STREQ("AndroidAutoInterface.usb", arg->list[i].string_value) {
            _enable_usb = true;
        } else if STREQ("AndroidAutoInterface.bluetooth", arg->list[i].string_value) {
            _enable_bluetooth = true;
        } else if STREQ("AndroidAutoInterface.wifi", arg->list[i].string_value) {
            _enable_wifi = true;
        }
    }

    if (_enable_usb && !aaplugin.usb_enabled) {
        ok = enable_usb(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    } else if (!_enable_usb && aaplugin.usb_enabled) {
        ok = disable_usb(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    }

    if (_enable_bluetooth && !aaplugin.bluetooth_enabled) {
        ok = enable_bluetooth(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    } else if (!_enable_bluetooth && aaplugin.bluetooth_enabled) {
        ok = disable_bluetooth(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    }

    if (_enable_wifi && !aaplugin.wifi_enabled) {
        ok = enable_wifi(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    } else if (!_enable_wifi && aaplugin.wifi_enabled) {
        ok = disable_wifi(&aaplugin);
        if (ok != 0) {
            return platch_respond_native_error_std(
                responsehandle,
                ok
            );
        }
    }

    return 0;
}

static int on_set_platform_information(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct std_value *t;
    char *headunit_name = NULL;
    char *car_model = NULL;
    char *car_year = NULL;
    char *car_serial = NULL;
    bool left_hand_drive_vehicle = false;
    char *headunit_manufacturer = NULL;
    char *headunit_model = NULL;
    char *sw_build = NULL;
    char *sw_version = NULL;
    bool can_play_native_media_during_vr = false;
    bool hide_clock = false;
    int ok;

    if (arg->type != kStdMap) {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a map."
        );
    }

    t = stdmap_get_str_const(arg, "headunitName");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        headunit_name = strdup(t->string_value);
        if (headunit_name == NULL) {
            return platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
        }
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['headunitName']` to be a string."
        );
    }

    t = stdmap_get_str_const(arg, "carModel");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        car_model = strdup(t->string_value);
        if (car_model == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['carModel']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "carYear");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        car_year = strdup(t->string_value);
        if (car_year == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['carYear']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "carSerial");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        car_serial = strdup(t->string_value);
        if (car_serial == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['carSerial']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "leftHandDriveVehicle");
    if ((t != NULL) && STDVALUE_IS_BOOL(*t)) {
        left_hand_drive_vehicle = STDVALUE_AS_BOOL(*t);
    } else {
        ok =  platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['leftHandDriveVehicle']` to be a boolean."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "headunitManufacturer");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        headunit_manufacturer = strdup(t->string_value);
        if (headunit_manufacturer == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['headunitManufacturer']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "headunitModel");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        headunit_model = strdup(t->string_value);
        if (headunit_model == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['headunitModel']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "swBuild");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        sw_build = strdup(t->string_value);
        if (sw_build == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['swBuild']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "swVersion");
    if ((t != NULL) && STDVALUE_IS_STRING(*t)) {
        sw_version = strdup(t->string_value);
        if (sw_version == NULL) {
            ok = platch_respond_native_error_std(
                responsehandle,
                ENOMEM
            );
            goto fail_free_strings;
        }
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['swVersion']` to be a string."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "canPlayNativeMediaDuringVR");
    if ((t != NULL) && STDVALUE_IS_BOOL(*t)) {
        can_play_native_media_during_vr = STDVALUE_AS_BOOL(*t);
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['canPlayNativeMediaDuringVR']` to be a boolean."
        );
        goto fail_free_strings;
    }

    t = stdmap_get_str_const(arg, "hideClock");
    if ((t != NULL) && STDVALUE_IS_BOOL(*t)) {
        hide_clock = STDVALUE_AS_BOOL(*t);
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['hideClock']` to be a boolean."
        );
        goto fail_free_strings;
    }

    platch_respond_success_std(
        responsehandle,
        &STDNULL
    );

    aaplugin.hu_info.headunit_name = headunit_name;
    aaplugin.hu_info.car_model = car_model;
    aaplugin.hu_info.car_year = car_year;
    aaplugin.hu_info.car_serial = car_serial;
    aaplugin.hu_info.left_hand_drive_vehicle = left_hand_drive_vehicle;
    aaplugin.hu_info.headunit_manufacturer = headunit_manufacturer;
    aaplugin.hu_info.headunit_model = headunit_model;
    aaplugin.hu_info.sw_build = sw_build;
    aaplugin.hu_info.sw_version = sw_version;
    aaplugin.hu_info.can_play_native_media_during_vr = can_play_native_media_during_vr;
    aaplugin.hu_info.hide_clock = hide_clock;

    return 0;


    fail_free_strings:
    if (headunit_name != NULL) free(headunit_name);
    if (car_model != NULL) free(car_model);
    if (car_year != NULL) free(car_year);
    if (car_serial != NULL) free(car_serial);
    if (headunit_manufacturer != NULL) free(headunit_manufacturer);
    if (headunit_model != NULL) free(headunit_model);
    if (sw_build != NULL) free(sw_build);
    if (sw_version != NULL) free(sw_version);

    fail_return_ok:
    return ok;
}

static int on_receive_method_channel(
    char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    int ok;

    (void) channel;

    if STREQ("setEnabledInterfaces", object->method) {
        ok = on_set_enabled_interfaces(&object->std_arg, responsehandle);
        if (ok != 0) {
            return ok;
        }
    } else if STREQ("setPlatformInformation", object->method) {
        ok = on_set_platform_information(&object->std_arg, responsehandle);
        if (ok != 0) {
            return ok;
        }
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

static int on_receive_event_channel(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) channel;

    if STREQ("listen", object->method) {
        aaplugin.event_channel_has_listener = true;
    } else if STREQ("cancel", object->method) {
        aaplugin.event_channel_has_listener = false;
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

int aaplugin_init(void) {
    int ok;

    ok = plugin_registry_set_receiver(ANDROID_AUTO_METHOD_CHANNEL, kStandardMethodCall, on_receive_method_channel);
    if (ok != 0) {
        return ok;
    }

    ok = plugin_registry_set_receiver(ANDROID_AUTO_EVENT_CHANNEL, kStandardMethodCall, on_receive_event_channel);
    if (ok != 0) {
        return ok;
    }

    ok = init_ssl(&aaplugin);
    if (ok != 0) {
        return ok;
    }

    ok = init_usb(&aaplugin);
    if (ok != 0) {
        return ok;
    }

    GError *err = NULL;
    ok = gst_init_check(NULL, NULL, &err);
    if (!ok) {
        if (err) {
            fprintf(stderr, "[android-auto plugin] Could not initialize gstreamer. gst_init_check: %s\n", err->message);
            g_error_free(err);
        } else {
            fprintf(stderr, "[android-auto plugin] Could not initialize gstreamer. gst_init_check: Unknown error\n");
        }
        return EINVAL;
    }

    return 0;
}

int aaplugin_deinit(void) {
    return 0;
}