#include "KittyScanner.hpp"
#include "KittyMemoryEx.hpp"

// refs
// https://github.com/learn-more/findpattern-bench

static bool compare(const char *data, const char *pattern, const char *mask)
{
    for (; *mask; ++mask, ++data, ++pattern)
    {
        if (*mask == 'x' && *data != *pattern)
            return false;
    }
    return !*mask;
}

static uintptr_t findInRange(const uintptr_t start, const uintptr_t end,
                             const char *pattern, const std::string &mask)
{
    const size_t scan_size = mask.length();

    if (scan_size < 1 || ((start + scan_size) > end))
        return 0;

    const size_t length = end - start;

    for (size_t i = 0; i < length; ++i)
    {
        const uintptr_t current_end = start + i + scan_size;
        if (current_end > end)
            break;

        if (!compare(reinterpret_cast<const char *>(start + i), pattern, mask.c_str()))
            continue;

        return start + i;
    }
    return 0;
}

std::vector<uintptr_t> KittyScannerMgr::findBytesAll(const uintptr_t start, const uintptr_t end,
                                                     const char *bytes, const std::string &mask) const
{
    std::vector<uintptr_t> local_list;

    if (!_pMem || start >= end || !bytes || mask.empty())
        return local_list;

    std::vector<char> buf(end - start);
    if (!_pMem->Read(start, &buf[0], buf.size()))
    {
        KITTY_LOGE("findBytesAll: failed to read into buffer.");
        return local_list;
    }

    uintptr_t curr_search_address = (uintptr_t)&buf[0];
    const size_t scan_size = mask.length();
    do
    {
        if (!local_list.empty())
            curr_search_address = local_list.back() + scan_size;

        uintptr_t found = findInRange(curr_search_address, (uintptr_t(&buf[0]) + buf.size()), bytes, mask);
        if (!found)
            break;

        local_list.push_back(found);
    } while (true);

    if (local_list.empty())
        return local_list;

    std::vector<uintptr_t> remote_list;
    for (auto &it : local_list)
    {
        remote_list.push_back((it - (uintptr_t(&buf[0]))) + start);
    }

    return remote_list;
}

uintptr_t KittyScannerMgr::findBytesFirst(const uintptr_t start, const uintptr_t end, const char *bytes, const std::string &mask) const
{
    if (!_pMem || start >= end || !bytes || mask.empty())
        return 0;

    std::vector<char> buf(end - start);
    if (_pMem->Read(start, &buf[0], buf.size()) != buf.size())
    {
        KITTY_LOGE("findBytesFirst: failed to read into buffer.");
        return 0;
    }

    uintptr_t local = findInRange((uintptr_t)&buf[0], (uintptr_t(&buf[0]) + buf.size()), bytes, mask);
    if (local)
        return (local - (uintptr_t(&buf[0]))) + start;

    return 0;
}

std::vector<uintptr_t> KittyScannerMgr::findHexAll(const uintptr_t start, const uintptr_t end, std::string hex, const std::string &mask) const
{
    std::vector<uintptr_t> list;

    if (!_pMem || start >= end || mask.empty() || !KittyUtils::validateHexString(hex))
        return list;

    const size_t scan_size = mask.length();
    if ((hex.length() / 2) != scan_size)
        return list;

    std::vector<char> pattern(scan_size);
    KittyUtils::dataFromHex(hex, &pattern[0]);

    list = findBytesAll(start, end, pattern.data(), mask);
    return list;
}

uintptr_t KittyScannerMgr::findHexFirst(const uintptr_t start, const uintptr_t end, std::string hex, const std::string &mask) const
{
    if (!_pMem || start >= end || mask.empty() || !KittyUtils::validateHexString(hex))
        return 0;

    const size_t scan_size = mask.length();
    if ((hex.length() / 2) != scan_size)
        return 0;

    std::vector<char> pattern(scan_size);
    KittyUtils::dataFromHex(hex, &pattern[0]);

    return findBytesFirst(start, end, pattern.data(), mask);
}

std::vector<uintptr_t> KittyScannerMgr::findDataAll(const uintptr_t start, const uintptr_t end, const void *data, size_t size) const
{
    std::vector<uintptr_t> list;

    if (!_pMem || start >= end || !data || size < 1)
        return list;

    std::string mask(size, 'x');

    list = findBytesAll(start, end, (const char *)data, mask);
    return list;
}

uintptr_t KittyScannerMgr::findDataFirst(const uintptr_t start, const uintptr_t end, const void *data, size_t size) const
{
    if (!_pMem || start >= end || !data || size < 1)
        return 0;

    std::string mask(size, 'x');

    return findBytesFirst(start, end, (const char *)data, mask);
}

/* ======================= ElfScanner ======================= */

// refs https://gist.github.com/resilar/24bb92087aaec5649c9a2afc0b4350c8

ElfScanner::ElfScanner(IKittyMemOp *pMem, uintptr_t elfBase)
{
    _pMem = nullptr;
    _elfBase = 0;
    _ehdr = {};
    _loads = 0;
    _loadBias = 0;
    _loadSize = 0;
    _stringTable = 0;
    _symbolTable = 0;
    _strsz = 0;
    _syment = 0;

    if (!pMem || !elfBase)
        return;

    _pMem = pMem;
    _elfBase = elfBase;

    // read ELF header
    if (!_pMem->Read(elfBase, &_ehdr, sizeof(_ehdr)))
    {
        KITTY_LOGD("ElfScanner: failed to read ELF (%p) header.", (void *)elfBase);
        return;
    }

    // verify ELF header
    if (memcmp(_ehdr.e_ident, "\177ELF", 4) != 0)
    {
        KITTY_LOGD("ElfScanner: (%p) is not a valid ELF.", (void *)elfBase);
        return;
    }

    // check ELF bit
    if (_ehdr.e_ident[EI_CLASS] != ELF_EICLASS_)
    {
        KITTY_LOGD("ElfScanner: ELF class mismatch (%p).", (void *)elfBase);
        return;
    }

    // check common header values
    if (!_ehdr.e_phnum || !_ehdr.e_phentsize || !_ehdr.e_shnum || !_ehdr.e_shentsize)
    {
        KITTY_LOGD("ElfScanner: Invalid header values (%p).", (void *)elfBase);
        return;
    }

    // read all program headers
    std::vector<char> phdrs_buf(_ehdr.e_phnum * _ehdr.e_phentsize);
    if (!_pMem->Read(elfBase + _ehdr.e_phoff, &phdrs_buf[0], phdrs_buf.size()))
    {
        KITTY_LOGD("ElfScanner: failed to read ELF (%p) program headers.", (void *)elfBase);
        return;
    }

    // find load bias
    uintptr_t min_vaddr = UINTPTR_MAX, max_vaddr = 0;
    for (ElfW_(Half) i = 0; i < _ehdr.e_phnum; i++)
    {
        ElfW_(Phdr) phdr_entry = {};
        memcpy(&phdr_entry, phdrs_buf.data() + (i * _ehdr.e_phentsize), _ehdr.e_phentsize);
        _phdrs.push_back(phdr_entry);

        if (phdr_entry.p_type == PT_LOAD)
        {
            _loads++;
            if (phdr_entry.p_vaddr < min_vaddr)
                min_vaddr = phdr_entry.p_vaddr;

            if (phdr_entry.p_vaddr + phdr_entry.p_memsz > max_vaddr)
                max_vaddr = phdr_entry.p_vaddr + phdr_entry.p_memsz;
        }
    }

    if (!_loads)
    {
        KITTY_LOGD("ElfScanner: No loads entry for ELF (%p).", (void *)elfBase);
        return;
    }

    if (!max_vaddr)
    {
        KITTY_LOGD("ElfScanner: failed to find load size for ELF (%p).", (void *)elfBase);
        return;
    }

    min_vaddr = KT_PAGE_START(min_vaddr);
    max_vaddr = KT_PAGE_END(max_vaddr);

    _loadBias = elfBase - min_vaddr;
    _loadSize = max_vaddr - min_vaddr;

    // read all dynamics
    for (auto &phdr : _phdrs)
    {
        if (phdr.p_type == PT_DYNAMIC)
        {
            uintptr_t dyn_addr = _loadBias + phdr.p_vaddr;
            std::vector<ElfW_(Dyn)> dyn_buff(phdr.p_memsz / sizeof(ElfW_(Dyn)));
            if (!_pMem->Read(dyn_addr, &dyn_buff[0], phdr.p_memsz))
            {
                KITTY_LOGD("ElfScanner: failed to read dynamic for ELF (%p).", (void *)elfBase);
                break;
            }

            for (auto &dyn : dyn_buff)
            {
                if (dyn.d_tag == DT_NULL)
                    break;

                // set required dynamics for symbol lookup
                switch (dyn.d_tag)
                {
                    // mandatory
                case DT_STRTAB: // string table
                    _stringTable = dyn.d_un.d_ptr;
                    break;
                    // mandatory
                case DT_SYMTAB: // symbol table
                    _symbolTable = dyn.d_un.d_ptr;
                    break;
                    // mandatory
                case DT_STRSZ: // string table size
                    _strsz = dyn.d_un.d_val;
                    break;
                    // mandatory
                case DT_SYMENT: // symbol entry size
                    _syment = dyn.d_un.d_val;
                    break;
                default:
                    break;
                }

                _dynamics.push_back(dyn);
            }
        }
    }

    // check required dynamics for symbol lookup
    if (!_stringTable || !_symbolTable || !_strsz || !_syment)
    {
        KITTY_LOGD("ElfScanner: failed to require dynamics for symbol lookup.");
        KITTY_LOGD("ElfScanner: elfBase: %p | strtab=%p | symtab=%p | strsz=%p | syment=%p",
                   (void *)elfBase, (void *)_stringTable, (void *)_symbolTable, (void *)_strsz, (void *)_syment);
        return;
    }

    auto fix_table_address = [&](uintptr_t &table_addr)
    {
        if (table_addr && table_addr < _loadBias)
            table_addr += _loadBias;
    };
    auto get_sym_address = [&](const ElfW_(Sym) & sym_ent) -> uintptr_t
    {
        return sym_ent.st_value < _loadBias ? _loadBias + sym_ent.st_value : sym_ent.st_value;
    };

    fix_table_address(_stringTable);
    fix_table_address(_symbolTable);

    // linear search
    uintptr_t sym_entry = _symbolTable;
    for (; sym_entry; sym_entry += _syment)
    {
        ElfW_(Sym) curr_sym{};
        if (!_pMem->Read(sym_entry, &curr_sym, _syment) || curr_sym.st_name >= _strsz)
            break;

        if (!curr_sym.st_name || !curr_sym.st_value)
            continue;

        std::string sym_str = _pMem->ReadStr(_stringTable + curr_sym.st_name, 1024);
        if (!sym_str.empty())
            _symbols.emplace_back(get_sym_address(curr_sym), sym_str);
    }
}

uintptr_t ElfScanner::findSymbol(const std::string &symbolName) const
{
    if (_symbols.empty())
        return 0;

    for (auto &sym : _symbols)
        if (!sym.second.empty() && sym.second == symbolName)
            return sym.first;

    return 0;
}