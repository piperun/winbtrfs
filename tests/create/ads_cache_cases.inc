/* CRC32C uses seed 0xfffffffe over the full user.<name> UTF-8 xattr.
 * equal length: adse585dd69ca30abba / ads7915a8e899062b68 = 0x930e70cc
 * unequal: ads0cf29dd4fd238ad1 / other554fc677dabbdbed = 0x9aa368a1
 * prefix: prefix / prefixfVKbGQ = 0x36a5fa3c */
#define ADS_EQUAL .stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"ads7915a8e899062b68",.stream_name=L"ads7915a8e899062b68",.colliding_xattr="adse585dd69ca30abba"
{.name="ads-cache-cold-equal",ADS_EQUAL,.cold_stream=true,.reopen_only=true},
{.name="ads-cache-cold-equal-reverse",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"adse585dd69ca30abba",.stream_name=L"adse585dd69ca30abba",.colliding_xattr="ads7915a8e899062b68",.cold_stream=true,.reopen_only=true},
{.name="ads-cache-cold-shorter",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"ads0cf29dd4fd238ad1",.stream_name=L"ads0cf29dd4fd238ad1",.colliding_xattr="other554fc677dabbdbed",.cold_stream=true,.reopen_only=true},
{.name="ads-cache-cold-longer",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"other554fc677dabbdbed",.stream_name=L"other554fc677dabbdbed",.colliding_xattr="ads0cf29dd4fd238ad1",.cold_stream=true,.reopen_only=true},
{.name="ads-cache-prefix-shorter",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"prefix",.stream_name=L"prefix",.colliding_xattr="prefixfVKbGQ",.cold_stream=true,.reopen_only=true},
{.name="ads-cache-prefix-longer",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"prefixfVKbGQ",.stream_name=L"prefixfVKbGQ",.colliding_xattr="prefix",.cold_stream=true,.reopen_only=true},
{.name="ads-cache-warm-before-target",ADS_EQUAL,.reopen_only=true},
{.name="ads-cache-warm-after-target",ADS_EQUAL,.reopen_only=true,.collider_after_target=true},
{.name="ads-cache-directory",ADS_EQUAL,.cold_stream=true,.reopen_only=true,.existing_directory=true},
{.name="ads-cache-paging",ADS_EQUAL,.cold_stream=true,.reopen_only=true,.paging=true},
{.name="ads-cache-casefold",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"ads7915a8e899062b68",.stream_name=L"ADS7915A8E899062B68",.colliding_xattr="adse585dd69ca30abba",.cold_stream=true,.reopen_only=true,.insensitive=true},
{.name="ads-cache-create-cold",ADS_EQUAL,.cold_stream=true,.expected=STATUS_OBJECT_NAME_COLLISION},
{.name="ads-cache-create-warm",ADS_EQUAL,.expected=STATUS_OBJECT_NAME_COLLISION},
{.name="ads-cache-dispatch-cold",ADS_EQUAL,.cold_stream=true,.dispatch=true,.expected=STATUS_OBJECT_NAME_COLLISION},
{.name="ads-cache-dispatch-warm",ADS_EQUAL,.dispatch=true,.expected=STATUS_OBJECT_NAME_COLLISION},
{.name="ads-cache-fileref-failure",ADS_EQUAL,.cold_stream=true,.reopen_only=true,.fault=FILEREF_ALLOC,.expected=STATUS_INSUFFICIENT_RESOURCES},
{.name="ads-cache-xattr-failure",ADS_EQUAL,.cold_stream=true,.reopen_only=true,.fault=XATTR_ERROR,.expected=STATUS_INTERNAL_ERROR},
{.name="ads-cache-metadata-failure",ADS_EQUAL,.cold_stream=true,.reopen_only=true,.fault=TREE_ERROR,.expected=STATUS_IO_DEVICE_ERROR},
{.name="ads-cache-publication",ADS_EQUAL,.reopen_only=true,.install_stream=true},
{.name="ads-cache-same-name-capacity",.stream=true,.existing_stream=true,.reaped_stream=true,.cached_stream_name=L"prefix",.stream_name=L"prefix",.colliding_xattr="prefix",.cold_stream=true,.reopen_only=true},
#undef ADS_EQUAL
