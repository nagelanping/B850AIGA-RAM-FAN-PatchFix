@echo -off
set FileName MSiCraftB850AIGA_E1.6D_011526.ROM
set FilePath \efi\boot
for %a run (0 20)
 fs%a:
cd %FilePath%
 if exist %FileName% then
AfuEfix64.efi %FileName% /p /fab /n
goto End
endif
endfor
echo Error: Bios not found.
:End
@echo -on