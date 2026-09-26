# Lenguaje C
1. Uso de epoll en Linux, kqueue en mac, IOCP para windows.
2. Redirección por nombre de dominio
3. Proxy Level 7
4. Podemos tener varias entradas en el proxy escuchado en diferentes puertos.
5. Cada entrada tendrá n salidas. La salida será en función del dominio.
6. Para cada saluda puede haber varios servers en round robin, o podrá balancear la carga a partir de indicadores de carga de los servidores clientes.
7. implementa keep-alive HTTP/1.1 de reutilización de conexión.

# Gestor del proyecto C
1. Manejar el proyecto con meson

# Recarga automática de la configuración.

#Test del funcionamiento del proxy
1. Generar datos de configuración
2. Lanza servidores de servicios
3. Dar de alta en /etc/hosts diferentes dominios.

#Benchmark
Haz un benchmark para 50000 requerimientos por segundo.
Haz el backend para probar en C con kqueue.


