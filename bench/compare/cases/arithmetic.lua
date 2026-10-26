return function(rounds, size, seed)
    local total = 0
    for _ = 1, rounds do
        local sum = seed
        local i = 0
        while i < size do
            if i < seed then sum = sum + (i + 3) else sum = sum + (i + 1) end
            i = i + 1
        end
        total = total + sum
    end
    return total
end
