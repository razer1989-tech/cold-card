/* hello.c - trivial chainload TEST TARGET (not part of the product).
 * Stands in for grubx64.efi during QEMU testing: proves the logger's
 * LoadImage/StartImage path works end to end. Prints and returns. */
#include <efi.h>
#include <efilib.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);
    Print(L"HELLOCHAIN: chainload target reached and running\r\n");
    return EFI_SUCCESS;
}
