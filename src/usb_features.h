#pragma once

// USB serial and ROM flashing remain enabled independently of MSC.
// SD/RAM mass storage is disabled unless a private experimental build opts in.
#ifndef PAPERDIY_EXPERIMENTAL_USB_MSC
#define PAPERDIY_EXPERIMENTAL_USB_MSC 0
#endif
#if PAPERDIY_EXPERIMENTAL_USB_MSC != 0 && PAPERDIY_EXPERIMENTAL_USB_MSC != 1
#error PAPERDIY_EXPERIMENTAL_USB_MSC must be 0 or 1
#endif
