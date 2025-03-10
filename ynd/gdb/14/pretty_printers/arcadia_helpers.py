def hasCowInString(type, char_type='char'):
    return 'S_' in type.keys() or f'TBasicStringStorage<{char_type}, std::__y1::char_traits<{char_type}>, false>' in type.keys()
