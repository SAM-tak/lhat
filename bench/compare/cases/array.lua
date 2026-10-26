return function(rounds, size, seed)
    local total = 0
    for _ = 1, rounds do
        local values = {}
        local i = 0
        while i < size do values[i + 1] = i + seed; i = i + 1 end
        for _ = 1, 4 do
            i = 0
            while i < size do
                local value = (values[i + 1] or 0) + 1
                values[i + 1] = value
                total = total + value
                i = i + 1
            end
        end
    end
    return total
end
