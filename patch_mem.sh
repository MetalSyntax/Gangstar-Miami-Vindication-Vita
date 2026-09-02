sed -i '' 's/sbrk_used = 0;/sbrk_used = 8;/g' source/reimpl/mem.c
sed -i '' 's/void \*ret = sbrk_heap + sbrk_used;/void \*ret = sbrk_heap + sbrk_used;/g' source/reimpl/mem.c
