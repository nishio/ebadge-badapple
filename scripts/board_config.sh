# Shared build default. Override only when deliberately restoring an older layout.
partition_scheme=${PARTITION_SCHEME:-custom}
fqbn="esp32:esp32:esp32s3:FlashSize=32M,PSRAM=opi,FlashMode=qio,PartitionScheme=$partition_scheme,USBMode=hwcdc,CDCOnBoot=cdc"
partition_csv=firmware/partitions/badge_32mb.csv
# A sketch that needs another layout ships firmware/partitions/<sketch>_32mb.csv.
if [ -n "${sketch:-}" ] && [ -f "firmware/partitions/${sketch}_32mb.csv" ]; then
  partition_csv="firmware/partitions/${sketch}_32mb.csv"
fi
